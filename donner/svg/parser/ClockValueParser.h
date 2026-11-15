#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/svg/components/animation/ClockValue.h"

namespace donner::svg::parser {

/**
 * Parses SMIL clock values from string representation.
 *
 * Supported formats per the SVG Animations spec:
 * - Full clock: `HH:MM:SS` or `HH:MM:SS.frac`
 * - Partial clock: `MM:SS` or `MM:SS.frac`
 * - Timecount: digits with an optional `.` fraction and an optional metric, `h`, `min`, `s` or
 *   `ms`; without a metric the value is in seconds
 * - `indefinite`
 *
 * Clock components are decimal digits only; the seconds component may carry a `.` fraction with at
 * least one digit on each side. For offsets, one optional `+` or `-` sign may precede the value,
 * and XML whitespace (space, tab, CR, LF) is allowed around the value and between the sign and the
 * value, e.g. `- 01:00`.
 *
 * Beyond the strict SMIL grammar, a timecount may use an exponent (`1e-3s`) or a leading dot
 * (`.5s`), the number forms of \ref donner::parser::NumberParser.
 */
class ClockValueParser {
public:
  /**
   * Parse a clock value string.
   *
   * @param str The string to parse.
   * @return The parsed ClockValue, or an error.
   */
  static ParseResult<components::ClockValue> Parse(std::string_view str);
};

}  // namespace donner::svg::parser
