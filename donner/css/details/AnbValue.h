#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::css {

/**
 * An+B microsyntax value, which is parsed by \ref parser::AnbMicrosyntaxParser.
 *
 * For example, the arguments of `:nth-child(4n+2)` are parsed as an AnbValue with a = 4, and b = 2.
 *
 * @see https://www.w3.org/TR/css-syntax-3/#anb-microsyntax
 */
struct AnbValue {
  int a = 0;  //!< The 'a' value in the An+B microsyntax.
  int b = 0;  //!< The 'b' value in the An+B microsyntax.

  /**
   * Evaluate whether the given child index equals A*n+B for a non-negative integer n.
   *
   * For example, if this AnbValue represents `4n+2`, then `evaluate(2)` would return true, but
   * `evaluate(3)` would return false.
   *
   * @param index A 1-based child index; `evaluate(1)` is the first child. If index is non-positive,
   * evaluate returns false.
   */
  int evaluate(int index) const noexcept {
    if (index <= 0) {
      return false;
    }

    if (a == 0) {
      return index == b;
    }

    // Widen before subtracting so extreme coefficients cannot overflow.
    const int64_t delta = int64_t(index) - b;
    return delta % a == 0 && delta / a >= 0;
  }

  /// Equality operator.
  bool operator==(const AnbValue& other) const noexcept = default;

  /// Print to an output stream.
  friend std::ostream& operator<<(std::ostream& os, const AnbValue& value) {
    return os << "AnbValue{ " << value.a << "n" << (value.b >= 0 ? "+" : "-") << std::abs(value.b)
              << " }";
  }
};

}  // namespace donner::css
