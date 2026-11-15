#include "donner/svg/parser/ClockValueParser.h"

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/ParseDiagnostic.h"
#include "donner/base/parser/NumberParser.h"

namespace donner::svg::parser {

namespace {

bool IsAsciiDigit(char ch) {
  return ch >= '0' && ch <= '9';
}

/// XML whitespace, which the SMIL offset grammar allows around a clock value and after its sign.
bool IsClockWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view TrimLeadingClockWhitespace(std::string_view str) {
  while (!str.empty() && IsClockWhitespace(str.front())) {
    str.remove_prefix(1);
  }
  return str;
}

std::string_view TrimClockValue(std::string_view str) {
  str = TrimLeadingClockWhitespace(str);
  while (!str.empty() && IsClockWhitespace(str.back())) {
    str.remove_suffix(1);
  }
  return str;
}

/// Parses the unsigned number that starts a timecount. The value must begin with a digit or '.',
/// since the offset sign is consumed once by \ref ClockValueParser::Parse.
std::optional<donner::parser::NumberParser::Result> ParseUnsignedNumber(std::string_view str) {
  if (str.empty() || !(IsAsciiDigit(str.front()) || str.front() == '.')) {
    return std::nullopt;
  }
  auto result = donner::parser::NumberParser::Parse(str);
  if (result.hasError()) {
    return std::nullopt;
  }
  return result.result();
}

/// Parses an `Hours` or `Minutes` clock component, `DIGIT+`, returning the number of characters
/// consumed or -1 on failure.
int ParseClockInteger(std::string_view str, int& out) {
  if (str.empty() || !IsAsciiDigit(str.front())) {
    return -1;
  }
  const char* begin = str.data();
  const char* end = begin + str.size();
  auto [ptr, ec] = std::from_chars(begin, end, out);
  if (ec != std::errc{}) {
    return -1;
  }
  return static_cast<int>(ptr - begin);
}

size_t CountLeadingDigits(std::string_view str) {
  size_t count = 0;
  while (count < str.size() && IsAsciiDigit(str[count])) {
    ++count;
  }
  return count;
}

/// Returns true if \p str is exactly `DIGIT+ ("." DIGIT+)?`.
bool IsDecimalWithOptionalFraction(std::string_view str) {
  const size_t integerDigits = CountLeadingDigits(str);
  if (integerDigits == 0) {
    return false;
  }
  if (integerDigits == str.size()) {
    return true;
  }
  const std::string_view fraction = str.substr(integerDigits + 1);
  return str[integerDigits] == '.' && !fraction.empty() &&
         CountLeadingDigits(fraction) == fraction.size();
}

/// Parses the whole of a `Seconds ("." Fraction)?` clock component.
std::optional<double> ParseClockSeconds(std::string_view str) {
  if (!IsDecimalWithOptionalFraction(str)) {
    return std::nullopt;
  }

  const auto number = ParseUnsignedNumber(str);
  if (!number.has_value() || number->consumedChars != str.size()) {
    return std::nullopt;
  }
  return number->number;
}

ParseDiagnostic InvalidClockFormat() {
  return ParseDiagnostic::Error("Invalid clock value format", FileOffset::Offset(0));
}

ParseResult<components::ClockValue> FinishClockValue(double seconds, bool negative) {
  if (!std::isfinite(seconds)) {
    return ParseDiagnostic::Error("Clock value is out of range", FileOffset::Offset(0));
  }
  return components::ClockValue::Seconds(negative ? -seconds : seconds);
}

/// Parses `Hours ":" Minutes ":" Seconds` or `Minutes ":" Seconds`, each with an optional
/// fraction on the seconds.
ParseResult<components::ClockValue> ParseColonClock(std::string_view str, bool negative) {
  int first = 0;
  int consumed = ParseClockInteger(str, first);
  if (consumed <= 0 || static_cast<size_t>(consumed) >= str.size() || str[consumed] != ':') {
    return InvalidClockFormat();
  }
  str.remove_prefix(consumed + 1);

  const size_t secondColon = str.find(':');
  if (secondColon == std::string_view::npos) {
    const std::optional<double> seconds = ParseClockSeconds(str);
    if (!seconds.has_value()) {
      return InvalidClockFormat();
    }
    return FinishClockValue(static_cast<double>(first) * 60.0 + *seconds, negative);
  }

  int minutes = 0;
  consumed = ParseClockInteger(str, minutes);
  if (consumed <= 0 || static_cast<size_t>(consumed) != secondColon) {
    return InvalidClockFormat();
  }
  const std::optional<double> seconds = ParseClockSeconds(str.substr(secondColon + 1));
  if (!seconds.has_value()) {
    return InvalidClockFormat();
  }
  return FinishClockValue(
      static_cast<double>(first) * 3600.0 + static_cast<double>(minutes) * 60.0 + *seconds,
      negative);
}

ParseResult<components::ClockValue> ParseTimecount(std::string_view str, bool negative) {
  const auto number = ParseUnsignedNumber(str);
  if (!number.has_value()) {
    return ParseDiagnostic::Error("Invalid clock value: expected a number", FileOffset::Offset(0));
  }

  const std::string_view suffix = str.substr(number->consumedChars);
  if (suffix.empty() || suffix == "s") {
    return FinishClockValue(number->number, negative);
  }
  if (suffix == "ms") {
    return FinishClockValue(number->number / 1000.0, negative);
  }
  if (suffix == "min") {
    return FinishClockValue(number->number * 60.0, negative);
  }
  if (suffix == "h") {
    return FinishClockValue(number->number * 3600.0, negative);
  }
  return ParseDiagnostic::Error(
      RcString("Invalid clock value metric: '" + std::string(suffix) + "'"), FileOffset::Offset(0));
}

}  // namespace

ParseResult<components::ClockValue> ClockValueParser::Parse(std::string_view str) {
  str = TrimClockValue(str);

  if (str.empty()) {
    ParseDiagnostic err;
    err.reason = "Empty clock value";
    return err;
  }

  // Check for "indefinite".
  if (str == "indefinite") {
    return components::ClockValue::Indefinite();
  }

  // Check for sign (negative offsets are allowed in begin/end values).
  bool negative = false;
  if (str.front() == '-') {
    negative = true;
    str.remove_prefix(1);
    str = TrimLeadingClockWhitespace(str);
  } else if (str.front() == '+') {
    str.remove_prefix(1);
    str = TrimLeadingClockWhitespace(str);
  }

  // Try to parse clock values with colons (full or partial clock).
  if (str.find(':') != std::string_view::npos) {
    return ParseColonClock(str, negative);
  }
  return ParseTimecount(str, negative);
}

}  // namespace donner::svg::parser
