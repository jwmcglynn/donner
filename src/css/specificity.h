#pragma once
/// @file

#include <compare>
#include <cstdint>
#include <ostream>

namespace donner::css {

/**
 * A CSS specificity value, as defined in https://www.w3.org/TR/selectors-4/#specificity-rules,
 * which is used during cascading to determine which style takes precedence.
 *
 * The specificity is a 3-tuple of integers, where the first integer is the most significant, plus a
 * few special values such as "!important" which override.
 *
 * The 3-tuple is created as follows:
 * - `a`: The number of ID selectors in the selector.
 * - `b`: The number of class selectors, attributes selectors, and pseudo-classes in the selector.
 * - `c`: The number of type selectors and pseudo-elements in the selector.
 *
 * For example, the selector `#id.class` has a specificity of (1, 1, 0), while `div > p` has a
 * specificity of (0, 0, 2).
 *
 * To construct from a 3-tuple:
 * ```
 * const Specificity spec = Specificity::FromABC(1, 2, 3);
 * ```
 *
 * To construct from "!important":
 * ```
 * const Specificity spec = Specificity::Important();
 * ```
 */
class Specificity {
public:
  /// Default constructor, creates a specificity of (0, 0, 0).
  constexpr Specificity() = default;

  /// Copy constructor.
  Specificity(const Specificity& other) = default;

  /// Move constructor.
  Specificity(Specificity&& other) = default;

  /// Assignment operator.
  Specificity& operator=(const Specificity& other) = default;

  /// Move assignment operator.
  Specificity& operator=(Specificity&& other) = default;

  /**
   * Ostream output operator.
   *
   * Example output:
   * ```
   * Specificity(1, 2, 3)
   * ```
   *
   * or
   * ```
   * Specificity(!important)
   * ```
   *
   * @param os Output stream.
   * @param obj Specificity to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const Specificity& obj) {
    os << "Specificity(";
    if (obj.special_ == SpecialType::Important) {
      os << "!important";
    } else if (obj.special_ == SpecialType::StyleAttribute) {
      os << "style (second highest)";
    } else {
      os << obj.a_ << ", " << obj.b_ << ", " << obj.c_;
    }
    return os << ")";
  }

  /**
   * Creates a specificity from the 3-tuple of integers.
   *
   * @param a The number of ID selectors in the selector.
   * @param b The number of class selectors, attributes selectors, and pseudo-classes in the
   * selector.
   * @param c The number of type selectors and pseudo-elements in the selector.
   */
  static constexpr Specificity FromABC(uint32_t a, uint32_t b, uint32_t c) {
    return Specificity(a, b, c);
  }

  /**
   * Creates a specificity for an `!important` declaration.
   */
  static constexpr Specificity Important() { return Specificity(SpecialType::Important); }

  /**
   * Creates a specificity for a style attribute.
   */
  static constexpr Specificity StyleAttribute() { return Specificity(SpecialType::StyleAttribute); }

  /**
   * Creates a specificity that overrides any other value, for overriding styles from the C++ API.
   */
  static constexpr Specificity Override() { return Specificity(SpecialType::Override); }

  /// Spaceship operator.
  auto operator<=>(const Specificity& other) const {
    if (special_ != other.special_) {
      return special_ <=> other.special_;
    } else if (a_ != other.a_) {
      return a_ <=> other.a_;
    } else if (b_ != other.b_) {
      return b_ <=> other.b_;
    } else {
      return c_ <=> other.c_;
    }
  }

  /// Equality operator, for gtest.
  bool operator==(const Specificity& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
  }

private:
  /**
   * Special values for specificity, which take precedence over the 3-tuple.
   *
   * The order of these values is important, since operator<=> considers later enum values to be
   * greater.
   */
  enum class SpecialType {
    None,            ///< No special value.
    StyleAttribute,  ///< Style attribute, second highest precedence in CSS.
    Important,       ///< `!important` declaration, highest precedence in CSS.
    Override         ///< Values set from C++ API, which overrides all other values.
  };

  /// Internal constructor for the 3-tuple.
  constexpr Specificity(uint32_t a, uint32_t b, uint32_t c) : a_(a), b_(b), c_(c) {}

  /// Internal constructor for special values.
  constexpr Specificity(SpecialType special) : special_(special) {}

  uint32_t a_ = 0;  ///< The number of ID selectors in the selector.
  uint32_t b_ = 0;  ///< The number of class selectors, attributes selectors, and pseudo-classes in
                    ///< the selector.
  uint32_t c_ = 0;  ///< The number of type selectors and pseudo-elements in the selector.
  SpecialType special_ = SpecialType::None;  ///< Special value, if any.
};

}  // namespace donner::css
