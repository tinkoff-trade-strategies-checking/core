#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "core/market/bar.hpp"

namespace core {

// Ошибки источника данных. Пустая серия ошибкой не является (вернётся пустой вектор).
class MarketDataError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
// Версии данных с таким id нет (или она не готова к чтению).
class SnapshotNotFound : public MarketDataError {
 public:
  using MarketDataError::MarketDataError;
};
// В версии данных нет такого инструмента.
class SecurityNotFound : public MarketDataError {
 public:
  using MarketDataError::MarketDataError;
};

// Источник рыночных данных. Реализации детерминированы (INV-4): один и тот же запрос к той же
// (неизменяемой, INV-5) версии данных даёт тот же результат в том же порядке.
// Источник отдаёт серию целиком; видимость баров по ходу симуляции (INV-1) обеспечивает BarCursor.
class BarSource {
 public:
  virtual ~BarSource() = default;

  // Бары по возрастанию tsOpenMs, index = 0..n-1. См. BarRequest о границах.
  // SnapshotNotFound, если версия не найдена или не готова; SecurityNotFound, если нет инструмента.
  virtual std::vector<Bar> load(const BarRequest& request) = 0;

  // Справочные данные инструмента. Исключения - как у load.
  virtual Security security(const std::string& snapshotId, const std::string& secid) = 0;
};

}  // namespace core
