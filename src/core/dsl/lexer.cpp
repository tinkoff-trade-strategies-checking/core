#include "lexer.hpp"

#include <cstdio>
#include <utility>

namespace core::dsl::detail {
namespace {

constexpr std::string_view kKeywords[] = {
    "param", "range",  "step",   "rule", "when",        "then", "because",
    "buy",   "sell",   "cancel", "lots", "equity",      "cash", "position",
    "all",   "market", "limit",  "stop", "take_profit", "day",  "gtc",
    "and",   "or",     "not",    "true", "false",       "na"};

bool isKeyword(std::string_view Word) {
  for (std::string_view Keyword : kKeywords) {
    if (Keyword == Word)
      return true;
  }
  return false;
}

bool isDigit(unsigned char C) { return C >= '0' && C <= '9'; }
bool isIdentStart(unsigned char C) {
  return (C >= 'a' && C <= 'z') || C == '_';
}
bool isIdentPart(unsigned char C) { return isIdentStart(C) || isDigit(C); }

} // namespace

Lexer::Lexer(std::string_view Text) : Source(Text) {
  // BOM в начале текста не считается символом: редакторы Windows добавляют его
  // сами.
  if (Source.size() >= 3 && static_cast<unsigned char>(Source[0]) == 0xEF &&
      static_cast<unsigned char>(Source[1]) == 0xBB &&
      static_cast<unsigned char>(Source[2]) == 0xBF) {
    Index = 3;
  }
}

unsigned char Lexer::peek(std::size_t Ahead) const {
  const std::size_t At = Index + Ahead;
  return At < Source.size() ? static_cast<unsigned char>(Source[At]) : 0;
}

std::size_t Lexer::sequenceLength(std::size_t At) const {
  const auto Byte = [&](std::size_t K) -> int {
    return At + K < Source.size() ? static_cast<unsigned char>(Source[At + K])
                                  : -1;
  };
  const int Lead = Byte(0);
  std::size_t Length = 0;
  int Lowest = 0x80, Highest = 0xBF; // допустимый диапазон второго байта
  if (Lead >= 0xC2 && Lead <= 0xDF) {
    Length = 2;
  } else if (Lead >= 0xE0 && Lead <= 0xEF) {
    Length = 3;
    if (Lead == 0xE0)
      Lowest = 0xA0; // без избыточных кодировок
    if (Lead == 0xED)
      Highest = 0x9F; // без суррогатов
  } else if (Lead >= 0xF0 && Lead <= 0xF4) {
    Length = 4;
    if (Lead == 0xF0)
      Lowest = 0x90;
    if (Lead == 0xF4)
      Highest = 0x8F; // не выше U+10FFFF
  } else {
    return 0;
  }
  if (Byte(1) < Lowest || Byte(1) > Highest)
    return 0;
  for (std::size_t K = 2; K < Length; ++K) {
    if (Byte(K) < 0x80 || Byte(K) > 0xBF)
      return 0;
  }
  return Length;
}

void Lexer::advance() {
  const unsigned char C = peek();
  if (C < 0x80) {
    ++Index;
    if (C == '\n') {
      ++Line;
      Column = 1;
    } else {
      ++Column;
    }
    return;
  }
  const std::size_t Length = sequenceLength(Index);
  if (Length == 0) {
    fail({Line, Column, Line, Column + 1},
         "недопустимая последовательность байтов UTF-8");
  }
  Index += Length;
  ++Column;
}

void Lexer::fail(const SourceRange &Range, std::string Message) const {
  throw SyntaxError{
      Diagnostic{"E-SYN", Level::Error, Range, std::move(Message), {}}};
}

void Lexer::skipTrivia() {
  while (!atEnd()) {
    const unsigned char C = peek();
    if (C == ' ' || C == '\t' || C == '\r' || C == '\n') {
      advance();
    } else if (C == '#' &&
               !InSubstitution) { // в подстановке `#` - не комментарий, а
                                  // недопустимый символ
      while (!atEnd() && peek() != '\n')
        advance();
    } else {
      return;
    }
  }
}

Token Lexer::scan() {
  skipTrivia();
  const Pos Start = pos();
  if (atEnd()) {
    return Token{TokenKind::End, "", false, {}, rangeFrom(Start)};
  }
  const unsigned char C = peek();
  if (isDigit(C))
    return lexNumber(Start);
  if (isIdentStart(C))
    return lexIdent(Start);
  if (C == '"') {
    if (InSubstitution) {
      // В выражении строк нет: кавычка здесь - это закрытие строки `because`, а
      // `}` пропущена.
      fail({SubstitutionOpen.Line, SubstitutionOpen.Column,
            SubstitutionOpen.Line, SubstitutionOpen.Column + 1},
           "подстановка не закрыта, ожидалась `}`");
    }
    return lexString(Start);
  }
  return lexPunct(Start);
}

Token Lexer::lexNumber(Pos Start) {
  const std::size_t First = Index;
  while (isDigit(peek()))
    advance();
  bool Integer = true;
  if (peek() == '.' && isDigit(peek(1))) { // `5..50` - это 5, `..`, 50
    Integer = false;
    advance();
    while (isDigit(peek()))
      advance();
  }
  return Token{TokenKind::Number,
               std::string(Source.substr(First, Index - First)),
               Integer,
               {},
               rangeFrom(Start)};
}

Token Lexer::lexIdent(Pos Start) {
  const std::size_t First = Index;
  while (isIdentPart(peek()))
    advance();
  std::string Word(Source.substr(First, Index - First));
  const TokenKind Kind =
      isKeyword(Word) ? TokenKind::Keyword : TokenKind::Ident;
  return Token{Kind, std::move(Word), false, {}, rangeFrom(Start)};
}

Token Lexer::lexPunct(Pos Start) {
  const unsigned char C = peek();
  const unsigned char Next = peek(1);
  std::size_t Length = 0;
  switch (C) {
  case '<':
  case '>':
  case '=':
    Length = Next == '=' ? 2 : 1;
    break;
  case '!':
    Length = Next == '=' ? 2 : 0; // одиночного `!` нет: отрицание - `not`
    break;
  case '.':
    Length = Next == '.' ? 2 : 0;
    break;
  case '+':
  case '-':
  case '*':
  case '/':
  case '(':
  case ')':
  case '[':
  case ']':
  case ',':
  case '%':
    Length = 1;
    break;
  case '}':
    Length = InSubstitution ? 1 : 0;
    break;
  default:
    break;
  }
  if (Length == 0) {
    const std::size_t First = Index;
    advance();
    const SourceRange Range = rangeFrom(Start);
    std::string Shown;
    if (C < 0x20 || C == 0x7F) {
      char Buffer[16];
      std::snprintf(Buffer, sizeof Buffer, "U+%04X", static_cast<unsigned>(C));
      Shown = Buffer;
    } else {
      Shown = "`" + std::string(Source.substr(First, Index - First)) + "`";
    }
    std::string Message = "недопустимый символ " + Shown;
    if (C >= 'A' && C <= 'Z')
      Message +=
          ": идентификаторы состоят из строчных латинских букв, цифр и `_`";
    if (C == '!')
      Message += ": отрицание пишется словом `not`";
    fail(Range, std::move(Message));
  }
  const std::size_t First = Index;
  for (std::size_t K = 0; K < Length; ++K)
    advance();
  return Token{TokenKind::Punct,
               std::string(Source.substr(First, Length)),
               false,
               {},
               rangeFrom(Start)};
}

Token Lexer::lexString(Pos Start) {
  const SourceRange Quote{Start.Line, Start.Column, Start.Line,
                          Start.Column + 1};
  Token Tok{TokenKind::String, "", false, {}, {}};
  std::string Text;
  const auto FlushText = [&] {
    if (!Text.empty()) {
      StringPart Part;
      Part.Text = std::move(Text);
      Tok.Parts.push_back(std::move(Part));
      Text.clear();
    }
  };

  advance(); // открывающая кавычка
  for (;;) {
    if (atEnd())
      fail(Quote, "строка не закрыта, ожидалась `\"`");
    const unsigned char C = peek();
    if (C == '"') {
      advance();
      break;
    }
    if (C == '\r') { // файл с CRLF: перевод строки внутри строки - один `\n`
      advance();
      continue;
    }
    if (C == '\\') {
      const Pos At = pos();
      advance();
      const unsigned char Escaped = peek();
      if (atEnd() || (Escaped != '"' && Escaped != '\\')) {
        if (!atEnd())
          advance();
        fail(rangeFrom(At), "недопустимая escape-последовательность, допустимы "
                            "`\\\"` и `\\\\`");
      }
      Text.push_back(static_cast<char>(Escaped));
      advance();
    } else if (C == '{' && peek(1) == '{') {
      advance();
      advance();
      Text.push_back('{');
    } else if (C == '}' && peek(1) == '}') {
      advance();
      advance();
      Text.push_back('}');
    } else if (C == '}') {
      const Pos At = pos();
      advance();
      fail(rangeFrom(At),
           "одиночная `}` в строке: скобка записывается как `}}`");
    } else if (C == '{') {
      FlushText();
      const Pos Open = pos();
      advance();
      StringPart Part;
      Part.IsExpr = true;
      InSubstitution = true;
      SubstitutionOpen = Open;
      for (;;) {
        Token Inner = scan();
        if (Inner.Kind == TokenKind::End) {
          fail({Open.Line, Open.Column, Open.Line, Open.Column + 1},
               "подстановка не закрыта, ожидалась `}`");
        }
        if (Inner.Kind == TokenKind::Punct && Inner.Text == "}") {
          Part.Range = rangeFrom(Open);
          Part.Tokens.push_back(
              Token{TokenKind::End, "", false, {}, Inner.Range});
          break;
        }
        Part.Tokens.push_back(std::move(Inner));
      }
      InSubstitution = false;
      Tok.Parts.push_back(std::move(Part));
    } else {
      const std::size_t First = Index;
      advance();
      Text.append(Source.substr(First, Index - First));
    }
  }
  FlushText();
  Tok.Range = rangeFrom(Start);
  return Tok;
}

} // namespace core::dsl::detail
