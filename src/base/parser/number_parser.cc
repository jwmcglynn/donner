#include "src/base/parser/number_parser.h"

#include <cmath>

#include "absl/strings/charconv.h"

namespace donner {

ParseResult<NumberParser::Result> NumberParser::Parse(std::string_view str, Options options) {
  NumberParser::Result result;
  auto begin = str.begin();

  if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
    // Detected a hex value, only parse the first 0.
    result.number = 0.0;
    result.consumed_chars = 1;
    return result;
  }

  bool foundPlus = false;
  if (str.starts_with('+')) {
    // from_chars does not support the '+' sign at the beginning, but the <number-token> spec
    // requires it. Detect and advance, but make sure that the result is positive to prevent
    // cases where both signs are supplied, for example "+-0".
    foundPlus = true;
    ++begin;
  }

  auto [strAdvance, ec] = absl::from_chars(
      begin, str.end(), result.number, absl::chars_format::fixed | absl::chars_format::scientific);

  if (ec == std::errc()) {
    // If the string started with a plus, it should not be negative.
    if (foundPlus && std::signbit(result.number)) {
      ParseError err;
      err.reason = "Failed to parse number: Invalid sign";
      err.offset = 1;  // The character after the '+'
      return err;
    }

    if (!std::isfinite(result.number)) {
      ParseError err;
      err.reason = "Failed to parse number: Not finite";
      err.offset = 0;
      return err;
    }

    result.consumed_chars = strAdvance - str.begin();

    // Ending with a period is out-of-spec, move the pointer backwards and so it is not consumed.
    if (str[result.consumed_chars - 1] == '.') {
      --result.consumed_chars;
    }

    return result;
  } else if (!options.forbid_out_of_range && ec == std::errc::result_out_of_range) {
    result.consumed_chars = strAdvance - str.begin();

    if (str.starts_with('-')) {
      result.number = -std::numeric_limits<double>::infinity();
    } else {
      result.number = std::numeric_limits<double>::infinity();
    }

    return result;
  } else {
    ParseError err;
    err.reason = "Failed to parse number";
    if (ec == std::errc::invalid_argument) {
      err.reason += ": Invalid argument";
    } else if (ec == std::errc::result_out_of_range) {
      err.reason += ": Out of range";
    }
    err.offset = strAdvance - str.begin();
    return err;
  }
}

}  // namespace donner
