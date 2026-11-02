#pragma once

#include <string>
#include <vector>

// Диагностики DSL (dsl.md, «Диагностики»): код, уровень, место, сообщение по-русски, исправление.

namespace core::dsl {

// Место в тексте. Строки и столбцы - с 1; столбец считается в кодовых точках UTF-8 (не в байтах),
// конец исключающий: столбец сразу после последнего символа диапазона.
struct SourceRange {
  int startLine = 0;
  int startColumn = 0;
  int endLine = 0;
  int endColumn = 0;

  friend bool operator==(const SourceRange&, const SourceRange&) = default;
};

enum class Level { Ok, Warning, Error };  // по возрастанию серьёзности

// Замена «диапазон -> текст». Вставка - пустой диапазон; удаление - пустой text.
struct TextEdit {
  SourceRange range;
  std::string text;
};

struct Diagnostic {
  std::string code;  // "E-SYN", "E-TYPE", "L001", ...
  Level level = Level::Error;
  SourceRange range;
  std::string message;
  std::vector<TextEdit> fix;  // пусто, если исправления нет
};

}  // namespace core::dsl
