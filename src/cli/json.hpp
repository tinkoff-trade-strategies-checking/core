#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/money/decimal.hpp"

namespace cli {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue {
  using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double,
                               std::string, JsonArray, JsonObject>;

  Storage Data;

  JsonValue() : Data(nullptr) {}
  JsonValue(std::nullptr_t) : Data(nullptr) {}
  JsonValue(bool Val) : Data(Val) {}
  JsonValue(int Val) : Data(static_cast<std::int64_t>(Val)) {}
  JsonValue(std::int64_t Val) : Data(Val) {}
  JsonValue(double Val) : Data(Val) {}
  JsonValue(const char *Val) : Data(std::string(Val)) {}
  JsonValue(std::string Val) : Data(std::move(Val)) {}
  JsonValue(JsonArray Val) : Data(std::move(Val)) {}
  JsonValue(JsonObject Val) : Data(std::move(Val)) {}

  bool isNull() const { return std::holds_alternative<std::nullptr_t>(Data); }
  bool isBool() const { return std::holds_alternative<bool>(Data); }
  bool isInt() const { return std::holds_alternative<std::int64_t>(Data); }
  bool isNumber() const {
    return std::holds_alternative<std::int64_t>(Data) ||
           std::holds_alternative<double>(Data);
  }
  bool isString() const { return std::holds_alternative<std::string>(Data); }
  bool isArray() const { return std::holds_alternative<JsonArray>(Data); }
  bool isObject() const { return std::holds_alternative<JsonObject>(Data); }

  bool asBool(std::string_view Context = "") const;
  std::int64_t asInt64(std::string_view Context = "") const;
  double asDouble(std::string_view Context = "") const;
  core::Decimal asDecimal(std::string_view Context = "") const;
  const std::string &asString(std::string_view Context = "") const;
  const JsonArray &asArray(std::string_view Context = "") const;
  const JsonObject &asObject(std::string_view Context = "") const;
};

const JsonValue *findField(const JsonObject &Obj, std::string_view Key);
const JsonValue &requireField(const JsonObject &Obj, std::string_view Key,
                              std::string_view Context = "");

JsonValue parseJson(std::string_view Input);
std::string dumpJson(const JsonValue &Val, int Indent = 2);

} // namespace cli
