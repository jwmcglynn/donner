#include "donner/svg/parser/ClockValueParser.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

#include "donner/base/ParseDiagnostic.h"

namespace donner::svg::parser {

namespace {

// Parse a non-negative double from a string_view, returning the number of chars consumed.
// Returns -1 on failure.
int parseDouble(std::string_view str, double& out) {
  if (str.empty()) {
    return -1;
  }

  // Use strtod since std::from_chars for double is not available on all platforms.
  // We need a null-terminated string.
  std::string temp(str);
  char* endPtr = nullptr;
  out = std::strtod(temp.c_str(), &endPtr);
  if (endPtr == temp.c_str() || !std::isfinite(out)) {
    return -1;
  }
  return static_cast<int>(endPtr - temp.c_str());
}

// Parse a non-negative integer from a string_view, returning the number of chars consumed.
int parseInt(std::string_view str, int& out) {
  if (str.empty()) {
    return -1;
  }
  const char* begin = str.data();
  const char* end = begin + str.size();
  auto [ptr, ec] = std::from_chars(begin, end, out);
  if (ec != std::errc{} || ptr == begin) {
    return -1;
  }
  return static_cast<int>(ptr - begin);
}

std::string_view TrimClockValue(std::string_view str) {
  while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) {
    str.remove_prefix(1);
  }
  while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) {
    str.remove_suffix(1);
  }
  return str;
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

ParseResult<components::ClockValue> ParseFullClock(int hours, int minutes, std::string_view seconds,
                                                   bool negative) {
  double parsedSeconds = 0.0;
  const int consumed = parseDouble(seconds, parsedSeconds);
  if (consumed <= 0 || static_cast<size_t>(consumed) != seconds.size()) {
    return InvalidClockFormat();
  }
  return FinishClockValue(
      static_cast<double>(hours) * 3600.0 + static_cast<double>(minutes) * 60.0 + parsedSeconds,
      negative);
}

ParseResult<components::ClockValue> ParsePartialClock(int minutes, int parsedSeconds,
                                                      std::string_view secondsText, int consumed,
                                                      bool negative) {
  double seconds = static_cast<double>(parsedSeconds);
  const std::string_view remaining = secondsText.substr(consumed);
  if (!remaining.empty()) {
    if (remaining.front() != '.') {
      return ParseDiagnostic::Error("Unexpected characters in clock value", FileOffset::Offset(0));
    }
    const int fractionalConsumed = parseDouble(secondsText, seconds);
    if (fractionalConsumed <= 0 || static_cast<size_t>(fractionalConsumed) != secondsText.size()) {
      return InvalidClockFormat();
    }
  }
  return FinishClockValue(static_cast<double>(minutes) * 60.0 + seconds, negative);
}

ParseResult<components::ClockValue> ParseColonClock(std::string_view str, bool negative) {
  int first = 0;
  int consumed = parseInt(str, first);
  if (consumed <= 0 || static_cast<size_t>(consumed) >= str.size() || str[consumed] != ':') {
    return InvalidClockFormat();
  }
  str.remove_prefix(consumed + 1);

  int second = 0;
  consumed = parseInt(str, second);
  if (consumed <= 0) {
    return InvalidClockFormat();
  }
  if (static_cast<size_t>(consumed) < str.size() && str[consumed] == ':') {
    return ParseFullClock(first, second, str.substr(consumed + 1), negative);
  }
  return ParsePartialClock(first, second, str, consumed, negative);
}

ParseResult<components::ClockValue> ParseTimecount(std::string_view str, bool negative) {
  double number = 0.0;
  const int consumed = parseDouble(str, number);
  if (consumed <= 0) {
    return ParseDiagnostic::Error("Invalid clock value: expected a number", FileOffset::Offset(0));
  }

  const std::string_view suffix = str.substr(consumed);
  if (suffix.empty() || suffix == "s") {
    return FinishClockValue(number, negative);
  }
  if (suffix == "ms") {
    return FinishClockValue(number / 1000.0, negative);
  }
  if (suffix == "min") {
    return FinishClockValue(number * 60.0, negative);
  }
  if (suffix == "h") {
    return FinishClockValue(number * 3600.0, negative);
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
  } else if (str.front() == '+') {
    str.remove_prefix(1);
  }

  // Try to parse clock values with colons (full or partial clock).
  if (str.find(':') != std::string_view::npos) {
    return ParseColonClock(str, negative);
  }
  return ParseTimecount(str, negative);
}

}  // namespace donner::svg::parser
