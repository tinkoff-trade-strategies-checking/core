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

bool isKeyword(std::string_view word) {
  for (std::string_view keyword : kKeywords) {
    if (keyword == word)
      return true;
  }
  return false;
}

bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool isIdentStart(unsigned char c) {
  return (c >= 'a' && c <= 'z') || c == '_';
}
bool isIdentPart(unsigned char c) { return isIdentStart(c) || isDigit(c); }

} // namespace

Lexer::Lexer(std::string_view source) : source_(source) {
  // BOM в начале текста не считается символом: редакторы Windows добавляют его
  // сами.
  if (source_.size() >= 3 && static_cast<unsigned char>(source_[0]) == 0xEF &&
      static_cast<unsigned char>(source_[1]) == 0xBB &&
      static_cast<unsigned char>(source_[2]) == 0xBF) {
    index_ = 3;
  }
}

unsigned char Lexer::peek(std::size_t ahead) const {
  const std::size_t at = index_ + ahead;
  return at < source_.size() ? static_cast<unsigned char>(source_[at]) : 0;
}

std::size_t Lexer::sequenceLength(std::size_t at) const {
  const auto byte = [&](std::size_t k) -> int {
    return at + k < source_.size() ? static_cast<unsigned char>(source_[at + k])
                                   : -1;
  };
  const int lead = byte(0);
  std::size_t length = 0;
  int lowest = 0x80, highest = 0xBF; // допустимый диапазон второго байта
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    if (lead == 0xE0)
      lowest = 0xA0; // без избыточных кодировок
    if (lead == 0xED)
      highest = 0x9F; // без суррогатов
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    if (lead == 0xF0)
      lowest = 0x90;
    if (lead == 0xF4)
      highest = 0x8F; // не выше U+10FFFF
  } else {
    return 0;
  }
  if (byte(1) < lowest || byte(1) > highest)
    return 0;
  for (std::size_t k = 2; k < length; ++k) {
    if (byte(k) < 0x80 || byte(k) > 0xBF)
      return 0;
  }
  return length;
}

void Lexer::advance() {
  const unsigned char c = peek();
  if (c < 0x80) {
    ++index_;
    if (c == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    return;
  }
  const std::size_t length = sequenceLength(index_);
  if (length == 0) {
    fail({line_, column_, line_, column_ + 1},
         "недопустимая последовательность байтов UTF-8");
  }
  index_ += length;
  ++column_;
}

void Lexer::fail(const SourceRange &range, std::string message) const {
  throw SyntaxError{
      Diagnostic{"E-SYN", Level::Error, range, std::move(message), {}}};
}

void Lexer::skipTrivia() {
  while (!atEnd()) {
    const unsigned char c = peek();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance();
    } else if (c == '#' &&
               !inSubstitution_) { // в подстановке `#` - не комментарий, а
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
  const Pos start = pos();
  if (atEnd()) {
    return Token{TokenKind::End, "", false, {}, rangeFrom(start)};
  }
  const unsigned char c = peek();
  if (isDigit(c))
    return lexNumber(start);
  if (isIdentStart(c))
    return lexIdent(start);
  if (c == '"') {
    if (inSubstitution_) {
      // В выражении строк нет: кавычка здесь - это закрытие строки `because`, а
      // `}` пропущена.
      fail({substitutionOpen_.line, substitutionOpen_.column,
            substitutionOpen_.line, substitutionOpen_.column + 1},
           "подстановка не закрыта, ожидалась `}`");
    }
    return lexString(start);
  }
  return lexPunct(start);
}

Token Lexer::lexNumber(Pos start) {
  const std::size_t first = index_;
  while (isDigit(peek()))
    advance();
  bool integer = true;
  if (peek() == '.' && isDigit(peek(1))) { // `5..50` - это 5, `..`, 50
    integer = false;
    advance();
    while (isDigit(peek()))
      advance();
  }
  return Token{TokenKind::Number,
               std::string(source_.substr(first, index_ - first)),
               integer,
               {},
               rangeFrom(start)};
}

Token Lexer::lexIdent(Pos start) {
  const std::size_t first = index_;
  while (isIdentPart(peek()))
    advance();
  std::string word(source_.substr(first, index_ - first));
  const TokenKind kind =
      isKeyword(word) ? TokenKind::Keyword : TokenKind::Ident;
  return Token{kind, std::move(word), false, {}, rangeFrom(start)};
}

Token Lexer::lexPunct(Pos start) {
  const unsigned char c = peek();
  const unsigned char next = peek(1);
  std::size_t length = 0;
  switch (c) {
  case '<':
  case '>':
  case '=':
    length = next == '=' ? 2 : 1;
    break;
  case '!':
    length = next == '=' ? 2 : 0; // одиночного `!` нет: отрицание - `not`
    break;
  case '.':
    length = next == '.' ? 2 : 0;
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
    length = 1;
    break;
  case '}':
    length = inSubstitution_ ? 1 : 0;
    break;
  default:
    break;
  }
  if (length == 0) {
    const std::size_t first = index_;
    advance();
    const SourceRange range = rangeFrom(start);
    std::string shown;
    if (c < 0x20 || c == 0x7F) {
      char buffer[16];
      std::snprintf(buffer, sizeof buffer, "U+%04X", static_cast<unsigned>(c));
      shown = buffer;
    } else {
      shown = "`" + std::string(source_.substr(first, index_ - first)) + "`";
    }
    std::string message = "недопустимый символ " + shown;
    if (c >= 'A' && c <= 'Z')
      message +=
          ": идентификаторы состоят из строчных латинских букв, цифр и `_`";
    if (c == '!')
      message += ": отрицание пишется словом `not`";
    fail(range, std::move(message));
  }
  const std::size_t first = index_;
  for (std::size_t k = 0; k < length; ++k)
    advance();
  return Token{TokenKind::Punct,
               std::string(source_.substr(first, length)),
               false,
               {},
               rangeFrom(start)};
}

Token Lexer::lexString(Pos start) {
  const SourceRange quote{start.line, start.column, start.line,
                          start.column + 1};
  Token token{TokenKind::String, "", false, {}, {}};
  std::string text;
  const auto flushText = [&] {
    if (!text.empty()) {
      StringPart part;
      part.text = std::move(text);
      token.parts.push_back(std::move(part));
      text.clear();
    }
  };

  advance(); // открывающая кавычка
  for (;;) {
    if (atEnd())
      fail(quote, "строка не закрыта, ожидалась `\"`");
    const unsigned char c = peek();
    if (c == '"') {
      advance();
      break;
    }
    if (c == '\r') { // файл с CRLF: перевод строки внутри строки - один `\n`
      advance();
      continue;
    }
    if (c == '\\') {
      const Pos at = pos();
      advance();
      const unsigned char escaped = peek();
      if (atEnd() || (escaped != '"' && escaped != '\\')) {
        if (!atEnd())
          advance();
        fail(rangeFrom(at), "недопустимая escape-последовательность, допустимы "
                            "`\\\"` и `\\\\`");
      }
      text.push_back(static_cast<char>(escaped));
      advance();
    } else if (c == '{' && peek(1) == '{') {
      advance();
      advance();
      text.push_back('{');
    } else if (c == '}' && peek(1) == '}') {
      advance();
      advance();
      text.push_back('}');
    } else if (c == '}') {
      const Pos at = pos();
      advance();
      fail(rangeFrom(at),
           "одиночная `}` в строке: скобка записывается как `}}`");
    } else if (c == '{') {
      flushText();
      const Pos open = pos();
      advance();
      StringPart part;
      part.isExpr = true;
      inSubstitution_ = true;
      substitutionOpen_ = open;
      for (;;) {
        Token inner = scan();
        if (inner.kind == TokenKind::End) {
          fail({open.line, open.column, open.line, open.column + 1},
               "подстановка не закрыта, ожидалась `}`");
        }
        if (inner.kind == TokenKind::Punct && inner.text == "}") {
          part.range = rangeFrom(open);
          part.tokens.push_back(
              Token{TokenKind::End, "", false, {}, inner.range});
          break;
        }
        part.tokens.push_back(std::move(inner));
      }
      inSubstitution_ = false;
      token.parts.push_back(std::move(part));
    } else {
      const std::size_t first = index_;
      advance();
      text.append(source_.substr(first, index_ - first));
    }
  }
  flushText();
  token.range = rangeFrom(start);
  return token;
}

} // namespace core::dsl::detail
