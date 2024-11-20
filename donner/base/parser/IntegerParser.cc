#include "donner/base/parser/IntegerParser.h"

#include <array>
#include <limits>

namespace donner::parser {

namespace {

/// Digits lookup table, mapping character to decimal value. Values are 0-15 if if the character is
/// a digit (decimal or hex), or 255 if the character if not a digit (indicating the end of the
/// number).
static constexpr std::array<unsigned char, 256> kLookupDigits = []() {
  std::array<unsigned char, 256> table;
  for (int i = 0; i < 256; ++i) {
    if (i >= '0' && i <= '9') {
      table[i] = static_cast<unsigned char>(i - '0');
    } else if (i >= 'A' && i <= 'F') {
      table[i] = static_cast<unsigned char>(i - 'A' + 10);
    } else if (i >= 'a' && i <= 'f') {
      table[i] = static_cast<unsigned char>(i - 'a' + 10);
    } else {
      table[i] = 255;
    }
  }

  return table;
}();

}  // namespace

ParseResult<IntegerParser::Result> IntegerParser::Parse(std::string_view str) {
  constexpr uint32_t kMaxDecimal = std::numeric_limits<uint32_t>::max() / 10;       // 429496729
  constexpr uint32_t kMaxDecimalDigit = std::numeric_limits<uint32_t>::max() % 10;  // 5

  uint32_t number = 0;
  for (size_t i = 0; i < str.size(); ++i) {
    const unsigned char digit = kLookupDigits[static_cast<unsigned char>(str[i])];
    if (digit >= 10) {
      if (i == 0) {
        ParseError err;
        err.reason = "Unexpected character parsing integer";
        return err;
      }

      Result result;
      result.number = number;
      result.consumedChars = i;
      return result;
    }

    // Detect overflow
    if (number > kMaxDecimal || (number == kMaxDecimal && digit > kMaxDecimalDigit)) {
      ParseError err;
      err.reason = "Integer overflow";
      err.location = FileOffset::Offset(i);
      return err;
    }

    number = number * 10 + digit;
  }

  if (str.empty()) {
    ParseError err;
    err.reason = "Unexpected end of string";
    return err;
  }

  Result result;
  result.number = number;
  result.consumedChars = str.size();
  return result;
}

ParseResult<IntegerParser::Result> IntegerParser::ParseHex(std::string_view str) {
  constexpr uint32_t kMaxHex = std::numeric_limits<uint32_t>::max() / 16;       // 268435455
  constexpr uint32_t kMaxHexDigit = std::numeric_limits<uint32_t>::max() % 16;  // 15

  uint32_t number = 0;
  for (size_t i = 0; i < str.size(); ++i) {
    const unsigned char digit = kLookupDigits[static_cast<unsigned char>(str[i])];
    if (digit == 0xFF) {
      if (i == 0) {
        ParseError err;
        err.reason = "Unexpected character parsing hex integer";
        return err;
      }

      Result result;
      result.number = number;
      result.consumedChars = i;
      return result;
    }

    // Detect overflow
    if (number > kMaxHex || (number == kMaxHex && digit > kMaxHexDigit)) {
      ParseError err;
      err.reason = "Integer overflow";
      err.location = FileOffset::Offset(i);
      return err;
    }

    number = number * 16 + digit;
  }

  if (str.empty()) {
    ParseError err;
    err.reason = "Unexpected end of string";
    return err;
  }

  Result result;
  result.number = number;
  result.consumedChars = str.size();
  return result;
}

}  // namespace donner::parser
