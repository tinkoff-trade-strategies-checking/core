#pragma once

// Внутренний заголовок области dsl: лексер для parser.cpp. Не часть публичного
// интерфейса.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsl/diagnostic.hpp"

namespace core::dsl::detail {

enum class TokenKind { End, Ident, Keyword, Number, String, Punct };

struct Token;

// Часть строки: текст либо подстановка `{выражение}`.
struct StringPart {
  bool IsExpr = false;
  std::string Text; // текст, экранирование снято
  std::vector<Token> Tokens; // подстановка: лексемы выражения, последняя - End
                             // на закрывающей `}`
  SourceRange Range; // подстановка: от `{` до `}` включительно
};

struct Token {
  TokenKind Kind = TokenKind::End;
  std::string
      Text; // Ident - имя; Keyword и Punct - написание; Number - исходный текст
  bool IsInteger = false; // Number без дробной части
  std::vector<StringPart>
      Parts; // только String; соседний текст склеен, пустого текста нет
  SourceRange Range;
};

// Первая лексическая ошибка прерывает разбор исключением; parse() превращает её
// в E-SYN.
struct SyntaxError {
  Diagnostic Diag;
};

// Лексер читает по одной лексеме (лениво): ошибка в конце текста не заслоняет
// ошибку в начале.
class Lexer {
public:
  explicit Lexer(std::string_view Source);

  // На конце текста возвращает End (и дальше тоже). Бросает SyntaxError.
  Token next() { return scan(); }

private:
  struct Pos {
    int Line;
    int Column;
  };

  Token scan();
  Token lexNumber(Pos Start);
  Token lexIdent(Pos Start);
  Token lexString(Pos Start);
  Token lexPunct(Pos Start);
  void skipTrivia();

  bool atEnd() const { return Index >= Source.size(); }
  unsigned char peek(std::size_t Ahead = 0) const;
  void advance(); // одна кодовая точка; проверяет UTF-8
  Pos pos() const { return {Line, Column}; }
  SourceRange rangeFrom(Pos Start) const {
    return {Start.Line, Start.Column, Line, Column};
  }
  [[noreturn]] void fail(const SourceRange &Range, std::string Message) const;
  std::size_t sequenceLength(std::size_t At) const; // 0 - недопустимый UTF-8

  std::string_view Source;
  std::size_t Index = 0;
  int Line = 1;
  int Column = 1;
  bool InSubstitution = false;
  Pos SubstitutionOpen{1, 1}; // где открыта текущая подстановка
};

} // namespace core::dsl::detail
