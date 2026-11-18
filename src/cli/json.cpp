#include "json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace cli {

namespace {

std::string ctxPrefix(std::string_view Context) {
  if (Context.empty())
    return "";
  return std::string(Context) + ": ";
}

class Parser {
public:
  explicit Parser(std::string_view Input) : Src(Input) {}

  JsonValue parseRoot() {
    skipWs();
    JsonValue Val = parseValue();
    skipWs();
    if (Pos != Src.size())
      throw error("лишние символы после JSON");
    return Val;
  }

private:
  std::string_view Src;
  std::size_t Pos = 0;

  std::runtime_error error(const std::string &Msg) const {
    return std::runtime_error("Ошибка JSON (позиция " + std::to_string(Pos) +
                              "): " + Msg);
  }

  void skipWs() {
    while (Pos < Src.size()) {
      const char Ch = Src[Pos];
      if (Ch == ' ' || Ch == '\t' || Ch == '\n' || Ch == '\r')
        ++Pos;
      else
        break;
    }
  }

  char peek() const {
    if (Pos >= Src.size())
      throw error("неожиданный конец ввода");
    return Src[Pos];
  }

  char get() {
    const char Ch = peek();
    ++Pos;
    return Ch;
  }

  void expect(char Expected) {
    const char Ch = get();
    if (Ch != Expected)
      throw error(std::string("ожидался символ '") + Expected + "'");
  }

  JsonValue parseValue() {
    skipWs();
    const char Ch = peek();
    if (Ch == 'n')
      return parseLiteral("null", JsonValue(nullptr));
    if (Ch == 't')
      return parseLiteral("true", JsonValue(true));
    if (Ch == 'f')
      return parseLiteral("false", JsonValue(false));
    if (Ch == '"')
      return JsonValue(parseString());
    if (Ch == '[')
      return JsonValue(parseArray());
    if (Ch == '{')
      return JsonValue(parseObject());
    if (Ch == '-' || (Ch >= '0' && Ch <= '9'))
      return parseNumber();
    throw error(std::string("неожиданный символ '") + Ch + "'");
  }

  JsonValue parseLiteral(std::string_view Lit, JsonValue Val) {
    if (Src.substr(Pos, Lit.size()) != Lit)
      throw error("ожидалось " + std::string(Lit));
    Pos += Lit.size();
    return Val;
  }

  std::string parseString() {
    expect('"');
    std::string Out;
    while (Pos < Src.size()) {
      const char Ch = get();
      if (Ch == '"')
        return Out;
      if (static_cast<unsigned char>(Ch) < 0x20)
        throw error("неэкранированный управляющий символ в строке");
      if (Ch != '\\') {
        Out.push_back(Ch);
        continue;
      }
      const char Esc = get();
      switch (Esc) {
      case '"':
      case '\\':
      case '/':
        Out.push_back(Esc);
        break;
      case 'b':
        Out.push_back('\b');
        break;
      case 'f':
        Out.push_back('\f');
        break;
      case 'n':
        Out.push_back('\n');
        break;
      case 'r':
        Out.push_back('\r');
        break;
      case 't':
        Out.push_back('\t');
        break;
      case 'u': {
        if (Pos + 4 > Src.size())
          throw error("неполный \\uXXXX");
        unsigned int Code = 0;
        for (int I = 0; I < 4; ++I) {
          const char Hex = get();
          Code <<= 4U;
          if (Hex >= '0' && Hex <= '9')
            Code |= static_cast<unsigned int>(Hex - '0');
          else if (Hex >= 'a' && Hex <= 'f')
            Code |= static_cast<unsigned int>(Hex - 'a' + 10);
          else if (Hex >= 'A' && Hex <= 'F')
            Code |= static_cast<unsigned int>(Hex - 'A' + 10);
          else
            throw error("некорректный символ в \\uXXXX");
        }
        if (Code <= 0x7F) {
          Out.push_back(static_cast<char>(Code));
        } else if (Code <= 0x7FF) {
          Out.push_back(static_cast<char>(0xC0U | (Code >> 6U)));
          Out.push_back(static_cast<char>(0x80U | (Code & 0x3FU)));
        } else {
          Out.push_back(static_cast<char>(0xE0U | (Code >> 12U)));
          Out.push_back(static_cast<char>(0x80U | ((Code >> 6U) & 0x3FU)));
          Out.push_back(static_cast<char>(0x80U | (Code & 0x3FU)));
        }
        break;
      }
      default:
        throw error("неизвестная escape-последовательность");
      }
    }
    throw error("незакрытая строка");
  }

  JsonValue parseNumber() {
    const std::size_t Start = Pos;
    if (Src[Pos] == '-')
      ++Pos;
    if (Pos >= Src.size() ||
        !std::isdigit(static_cast<unsigned char>(Src[Pos])))
      throw error("некорректное число");
    if (Src[Pos] == '0') {
      ++Pos;
    } else {
      while (Pos < Src.size() &&
             std::isdigit(static_cast<unsigned char>(Src[Pos])))
        ++Pos;
    }
    bool IsFloat = false;
    if (Pos < Src.size() && Src[Pos] == '.') {
      IsFloat = true;
      ++Pos;
      if (Pos >= Src.size() ||
          !std::isdigit(static_cast<unsigned char>(Src[Pos])))
        throw error("ожидались цифры после '.'");
      while (Pos < Src.size() &&
             std::isdigit(static_cast<unsigned char>(Src[Pos])))
        ++Pos;
    }
    if (Pos < Src.size() && (Src[Pos] == 'e' || Src[Pos] == 'E')) {
      IsFloat = true;
      ++Pos;
      if (Pos < Src.size() && (Src[Pos] == '+' || Src[Pos] == '-'))
        ++Pos;
      if (Pos >= Src.size() ||
          !std::isdigit(static_cast<unsigned char>(Src[Pos])))
        throw error("ожидались цифры в показателе степени");
      while (Pos < Src.size() &&
             std::isdigit(static_cast<unsigned char>(Src[Pos])))
        ++Pos;
    }
    const std::string_view NumStr = Src.substr(Start, Pos - Start);
    if (!IsFloat) {
      std::int64_t IntVal = 0;
      const auto [Ptr, Ec] =
          std::from_chars(NumStr.data(), NumStr.data() + NumStr.size(), IntVal);
      if (Ec == std::errc() && Ptr == NumStr.data() + NumStr.size())
        return JsonValue(IntVal);
    }
    std::string Copy(NumStr);
    std::size_t Parsed = 0;
    const double DblVal = std::stod(Copy, &Parsed);
    if (Parsed != Copy.size() || !std::isfinite(DblVal))
      throw error("число вне диапазона");
    return JsonValue(DblVal);
  }

  JsonArray parseArray() {
    expect('[');
    JsonArray Arr;
    skipWs();
    if (peek() == ']') {
      get();
      return Arr;
    }
    while (true) {
      Arr.push_back(parseValue());
      skipWs();
      const char Ch = get();
      if (Ch == ']')
        return Arr;
      if (Ch != ',')
        throw error("ожидалась ',' или ']' в массиве");
    }
  }

  JsonObject parseObject() {
    expect('{');
    JsonObject Obj;
    skipWs();
    if (peek() == '}') {
      get();
      return Obj;
    }
    while (true) {
      skipWs();
      if (peek() != '"')
        throw error("ожидался строковый ключ объекта");
      std::string Key = parseString();
      skipWs();
      expect(':');
      Obj.insert_or_assign(std::move(Key), parseValue());
      skipWs();
      const char Ch = get();
      if (Ch == '}')
        return Obj;
      if (Ch != ',')
        throw error("ожидалась ',' или '}' в объекте");
    }
  }
};

void writeEscaped(std::ostringstream &Out, std::string_view Str) {
  Out << '"';
  for (const unsigned char Ch : Str) {
    switch (Ch) {
    case '"':
      Out << "\\\"";
      break;
    case '\\':
      Out << "\\\\";
      break;
    case '\b':
      Out << "\\b";
      break;
    case '\f':
      Out << "\\f";
      break;
    case '\n':
      Out << "\\n";
      break;
    case '\r':
      Out << "\\r";
      break;
    case '\t':
      Out << "\\t";
      break;
    default:
      if (Ch < 0x20) {
        Out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<unsigned int>(Ch) << std::dec;
      } else {
        Out << static_cast<char>(Ch);
      }
      break;
    }
  }
  Out << '"';
}

void dumpImpl(std::ostringstream &Out, const JsonValue &Val, int Indent,
              int Depth) {
  if (Val.isNull()) {
    Out << "null";
  } else if (Val.isBool()) {
    Out << (Val.asBool() ? "true" : "false");
  } else if (Val.isInt()) {
    Out << Val.asInt64();
  } else if (Val.isNumber()) {
    const double Num = Val.asDouble();
    if (!std::isfinite(Num))
      Out << "null";
    else
      Out << std::setprecision(15) << Num;
  } else if (Val.isString()) {
    writeEscaped(Out, Val.asString());
  } else if (Val.isArray()) {
    const JsonArray &Arr = Val.asArray();
    if (Arr.empty()) {
      Out << "[]";
      return;
    }
    Out << "[\n";
    for (std::size_t I = 0; I < Arr.size(); ++I) {
      Out << std::string(static_cast<std::size_t>((Depth + 1) * Indent), ' ');
      dumpImpl(Out, Arr[I], Indent, Depth + 1);
      if (I + 1 < Arr.size())
        Out << ',';
      Out << '\n';
    }
    Out << std::string(static_cast<std::size_t>(Depth * Indent), ' ') << ']';
  } else {
    const JsonObject &Obj = Val.asObject();
    if (Obj.empty()) {
      Out << "{}";
      return;
    }
    Out << "{\n";
    std::size_t I = 0;
    for (const auto &[Key, Child] : Obj) {
      Out << std::string(static_cast<std::size_t>((Depth + 1) * Indent), ' ');
      writeEscaped(Out, Key);
      Out << ": ";
      dumpImpl(Out, Child, Indent, Depth + 1);
      if (++I < Obj.size())
        Out << ',';
      Out << '\n';
    }
    Out << std::string(static_cast<std::size_t>(Depth * Indent), ' ') << '}';
  }
}

} // namespace

bool JsonValue::asBool(std::string_view Context) const {
  if (!isBool())
    throw std::runtime_error(ctxPrefix(Context) + "ожидался bool");
  return std::get<bool>(Data);
}

std::int64_t JsonValue::asInt64(std::string_view Context) const {
  if (!isInt())
    throw std::runtime_error(ctxPrefix(Context) + "ожидалось целое число");
  return std::get<std::int64_t>(Data);
}

double JsonValue::asDouble(std::string_view Context) const {
  if (isInt())
    return static_cast<double>(std::get<std::int64_t>(Data));
  if (std::holds_alternative<double>(Data))
    return std::get<double>(Data);
  throw std::runtime_error(ctxPrefix(Context) + "ожидалось число");
}

core::Decimal JsonValue::asDecimal(std::string_view Context) const {
  if (isString())
    return core::Decimal::parse(std::get<std::string>(Data));
  if (isInt())
    return core::Decimal::fromUnits(std::get<std::int64_t>(Data));
  if (std::holds_alternative<double>(Data)) {
    std::ostringstream Ss;
    Ss << std::fixed << std::setprecision(core::Decimal::kScale)
       << std::get<double>(Data);
    std::string Str = Ss.str();
    const std::size_t Dot = Str.find('.');
    if (Dot != std::string::npos) {
      while (Str.size() > Dot + 1 && Str.back() == '0')
        Str.pop_back();
      if (Str.back() == '.')
        Str.pop_back();
    }
    return core::Decimal::parse(Str);
  }
  throw std::runtime_error(ctxPrefix(Context) +
                           "ожидалась строка или число для Decimal");
}

const std::string &JsonValue::asString(std::string_view Context) const {
  if (!isString())
    throw std::runtime_error(ctxPrefix(Context) + "ожидалась строка");
  return std::get<std::string>(Data);
}

const JsonArray &JsonValue::asArray(std::string_view Context) const {
  if (!isArray())
    throw std::runtime_error(ctxPrefix(Context) + "ожидался массив");
  return std::get<JsonArray>(Data);
}

const JsonObject &JsonValue::asObject(std::string_view Context) const {
  if (!isObject())
    throw std::runtime_error(ctxPrefix(Context) + "ожидался объект");
  return std::get<JsonObject>(Data);
}

const JsonValue *findField(const JsonObject &Obj, std::string_view Key) {
  const auto It = Obj.find(Key);
  return It == Obj.end() ? nullptr : &It->second;
}

const JsonValue &requireField(const JsonObject &Obj, std::string_view Key,
                              std::string_view Context) {
  const JsonValue *Val = findField(Obj, Key);
  if (!Val)
    throw std::runtime_error(ctxPrefix(Context) + "отсутствует поле '" +
                             std::string(Key) + "'");
  return *Val;
}

JsonValue parseJson(std::string_view Input) {
  return Parser(Input).parseRoot();
}

std::string dumpJson(const JsonValue &Val, int Indent) {
  std::ostringstream Out;
  dumpImpl(Out, Val, Indent, 0);
  return Out.str();
}

} // namespace cli
