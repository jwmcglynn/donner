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
 * - Timecount: `<number>h`, `<number>min`, `<number>s`, `<number>ms`
 * - Bare number: interpreted as seconds
 * - `indefinite`
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
