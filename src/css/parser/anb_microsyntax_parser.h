#pragma once
/// @file

#include <iostream>

#include "src/base/parser/parse_result.h"
#include "src/css/component_value.h"

namespace donner::css {

struct AnbValue {
  int a = 0;
  int b = 0;

  int evaluate(int index) const noexcept {
    std::cout << "AnbValue::evaluate(" << index << ") = matching " << a << "n+" << b << "\n";
    // Return true if index matches any combination of a*n+b
    if (a == 0) {
      std::cout << "-> " << (index == b ? "true\n" : "false\n");
      return index == b;
    } else {
      std::cout << "-> " << ((index - b) % a == 0 ? "true\n" : "false\n");
      return (index - b) % a == 0;
    }
  }

  bool operator==(const AnbValue& other) const noexcept = default;

  friend std::ostream& operator<<(std::ostream& os, const AnbValue& value) {
    return os << "AnbValue{ " << value.a << "n" << (value.b >= 0 ? "+" : "-") << std::abs(value.b)
              << " }";
  }
};

/**
 * Parse a CSS value, per https://www.w3.org/TR/css-syntax-3/#parse-list-of-component-values. This
 * is used when parsing CSS-like attributes within XML/HTML, such as SVG presentation attributes.
 */
class AnbMicrosyntaxParser {
public:
  struct Result {
    AnbValue value;
    std::span<const ComponentValue> remainingComponents;
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
