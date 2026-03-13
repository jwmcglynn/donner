#include "donner/svg/parser/ClockValueParser.h"

#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>

#include "donner/base/ParseError.h"

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
  if (endPtr == temp.c_str()) {
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

}  // namespace

ParseResult<components::ClockValue> ClockValueParser::Parse(std::string_view str) {
  // Trim leading/trailing whitespace.
  while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) {
    str.remove_prefix(1);
  }
  while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) {
    str.remove_suffix(1);
  }

  if (str.empty()) {
    ParseError err;
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
    // Could be HH:MM:SS.frac or MM:SS.frac
    int first = 0;
    int consumed = parseInt(str, first);
    if (consumed <= 0 || static_cast<size_t>(consumed) >= str.size() || str[consumed] != ':') {
      ParseError err;
      err.reason = "Invalid clock value format";
      return err;
    }
    str.remove_prefix(consumed + 1);  // skip past ':'

    int second = 0;
    consumed = parseInt(str, second);
    if (consumed <= 0) {
      ParseError err;
      err.reason = "Invalid clock value format";
      return err;
    }

    if (static_cast<size_t>(consumed) < str.size() && str[consumed] == ':') {
      // Full clock: HH:MM:SS.frac
      str.remove_prefix(consumed + 1);
      double seconds = 0.0;
      consumed = parseDouble(str, seconds);
      if (consumed <= 0 || static_cast<size_t>(consumed) != str.size()) {
        ParseError err;
        err.reason = "Invalid clock value format";
        return err;
      }
      double total = static_cast<double>(first) * 3600.0 + static_cast<double>(second) * 60.0 +
                     seconds;
      return components::ClockValue::Seconds(negative ? -total : total);
    }

    // Partial clock: MM:SS.frac
    double seconds = 0.0;
    std::string_view remaining = str.substr(consumed);
    if (remaining.empty()) {
      seconds = static_cast<double>(second);
    } else if (remaining.front() == '.') {
      // MM:SS.frac - parse the full SS.frac from current position
      double fullSeconds = 0.0;
      consumed = parseDouble(str, fullSeconds);
      if (consumed <= 0 || static_cast<size_t>(consumed) != str.size()) {
        ParseError err;
        err.reason = "Invalid clock value format";
        return err;
      }
      seconds = fullSeconds;
    } else {
      ParseError err;
      err.reason = "Unexpected characters in clock value";
      return err;
    }
    double total = static_cast<double>(first) * 60.0 + seconds;
    return components::ClockValue::Seconds(negative ? -total : total);
  }

  // Timecount value: <number><metric> or bare number (seconds).
  double number = 0.0;
  int consumed = parseDouble(str, number);
  if (consumed <= 0) {
    ParseError err;
    err.reason = "Invalid clock value: expected a number";
    return err;
  }

  std::string_view suffix = str.substr(consumed);
  double seconds = 0.0;

  if (suffix.empty() || suffix == "s") {
    seconds = number;
  } else if (suffix == "ms") {
    seconds = number / 1000.0;
  } else if (suffix == "min") {
    seconds = number * 60.0;
  } else if (suffix == "h") {
    seconds = number * 3600.0;
  } else {
    ParseError err;
    err.reason = "Invalid clock value metric: '" + std::string(suffix) + "'";
    return err;
  }

  return components::ClockValue::Seconds(negative ? -seconds : seconds);
}

}  // namespace donner::svg::parser
