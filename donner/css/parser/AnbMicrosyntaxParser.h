#pragma once
/// @file

#include "donner/base/ParseResult.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/details/AnbValue.h"

namespace donner::css::parser {

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

    /// Ostream operator.
    friend std::ostream& operator<<(std::ostream& out, const Result& result) {
      return out << result.value << ", " << result.remainingComponents.size()
                 << " remaining components";
    }
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

}  // namespace donner::css::parser
