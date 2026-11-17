#pragma once

#include <string>
#include <vector>

// Диагностики DSL (dsl.md, «Диагностики»): код, уровень, место, сообщение
// по-русски, исправление.

namespace core::dsl {

// Место в тексте. Строки и столбцы - с 1; столбец считается в кодовых точках
// UTF-8 (не в байтах), конец исключающий: столбец сразу после последнего
// символа диапазона.
struct SourceRange {
  int StartLine = 0;
  int StartColumn = 0;
  int EndLine = 0;
  int EndColumn = 0;

  friend bool operator==(const SourceRange &, const SourceRange &) = default;
};

enum class Level { Ok, Warning, Error }; // по возрастанию серьёзности

// Замена «диапазон -> текст». Вставка - пустой диапазон; удаление - пустой
// text.
struct TextEdit {
  SourceRange Range;
  std::string Text;
};

struct Diagnostic {
  std::string Code; // "E-SYN", "E-TYPE", "L001", ...
  Level Severity = Level::Error;
  SourceRange Range;
  std::string Message;
  std::vector<TextEdit> Fix; // пусто, если исправления нет
};

} // namespace core::dsl
