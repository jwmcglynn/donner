#pragma once
/// @file

#include "src/base/parser/parse_result.h"
#include "src/css/component_value.h"
#include "src/css/details/anb_value.h"

namespace donner::css {

/**
 * Parse a CSS value, per https://www.w3.org/TR/css-syntax-3/#parse-list-of-component-values. This
 * is used when parsing CSS-like attributes within XML/HTML, such as SVG presentation attributes.
 */
class AnbMicrosyntaxParser {
public:
  /// Result of parsing the An+B microsyntax.
  struct Result {
    AnbValue value;  //!< The parsed An+B value.
    std::span<const ComponentValue>
        remainingComponents;  //!< The remaining components after the end of the An+B value, if not
                              //!< all were consumed.
  };

  /**
   * Parse the CSS An+B microsyntax, per https://www.w3.org/TR/css-syntax-3/#anb-microsyntax
   *
   * For example:
   * - "5" would return (0, 5)
   * - "odd" would return (1, 2)
   * - "even" would return (2, 2)
   * - "3n+1" would return (3, 1)
   *
   * @param components List of component values to parse.
   * @return Parsed value as a list of component values.
   */
  static ParseResult<Result> Parse(std::span<const ComponentValue> components);
};

}  // namespace donner::css