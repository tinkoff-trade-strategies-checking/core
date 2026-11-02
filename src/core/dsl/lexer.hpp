#pragma once

// Внутренний заголовок области dsl: лексер для parser.cpp. Не часть публичного интерфейса.

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
  bool isExpr = false;
  std::string text;           // текст, экранирование снято
  std::vector<Token> tokens;  // подстановка: лексемы выражения, последняя - End на закрывающей `}`
  SourceRange range;          // подстановка: от `{` до `}` включительно
};

struct Token {
  TokenKind kind = TokenKind::End;
  std::string text;               // Ident - имя; Keyword и Punct - написание; Number - исходный текст
  bool isInteger = false;         // Number без дробной части
  std::vector<StringPart> parts;  // только String; соседний текст склеен, пустого текста нет
  SourceRange range;
};

// Первая лексическая ошибка прерывает разбор исключением; parse() превращает её в E-SYN.
struct SyntaxError {
  Diagnostic diagnostic;
};

// Лексер читает по одной лексеме (лениво): ошибка в конце текста не заслоняет ошибку в начале.
class Lexer {
 public:
  explicit Lexer(std::string_view source);

  // На конце текста возвращает End (и дальше тоже). Бросает SyntaxError.
  Token next() { return scan(); }

 private:
  struct Pos {
    int line;
    int column;
  };

  Token scan();
  Token lexNumber(Pos start);
  Token lexIdent(Pos start);
  Token lexString(Pos start);
  Token lexPunct(Pos start);
  void skipTrivia();

  bool atEnd() const { return index_ >= source_.size(); }
  unsigned char peek(std::size_t ahead = 0) const;
  void advance();  // одна кодовая точка; проверяет UTF-8
  Pos pos() const { return {line_, column_}; }
  SourceRange rangeFrom(Pos start) const { return {start.line, start.column, line_, column_}; }
  [[noreturn]] void fail(const SourceRange& range, std::string message) const;
  std::size_t sequenceLength(std::size_t at) const;  // 0 - недопустимый UTF-8

  std::string_view source_;
  std::size_t index_ = 0;
  int line_ = 1;
  int column_ = 1;
  bool inSubstitution_ = false;
  Pos substitutionOpen_{1, 1};  // где открыта текущая подстановка
};

}  // namespace core::dsl::detail
