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
  /// A 3-tuple of integers representing the specificity before modifiers such as the "!important"
  /// flag have been applied.
  struct ABC {
    uint32_t a = 0;  ///< The number of ID selectors in the selector.
    uint32_t b = 0;  ///< The number of class selectors, attributes selectors, and pseudo-classes in
                     ///< the selector.
    uint32_t c = 0;  ///< The number of type selectors and pseudo-elements in the selector.

    /// Comparison operator.
    auto operator<=>(const ABC& other) const {
      if (a != other.a) {
        return a <=> other.a;
      } else if (b != other.b) {
        return b <=> other.b;
      } else {
        return c <=> other.c;
      }
    }
  };

  /// Default constructor, creates a specificity of (0, 0, 0).
  constexpr Specificity() = default;

  /// Constructs a specificity from a \ref ABC 3-tuple.
  explicit constexpr Specificity(const ABC& abc) : abc_(abc) {}

  /// Destructor.
  ~Specificity() = default;

  /// Copy constructor.
  Specificity(const Specificity& other) = default;

  /// Move constructor.
  Specificity(Specificity&& other) noexcept = default;

  /// Assignment operator.
  Specificity& operator=(const Specificity& other) = default;

  /// Move assignment operator.
  Specificity& operator=(Specificity&& other) noexcept = default;

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
      os << obj.abc_.a << ", " << obj.abc_.b << ", " << obj.abc_.c;
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
    return Specificity(ABC{a, b, c});
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
    } else {
      return abc_ <=> other.abc_;
    }
  }

  /// Equality operator, for gtest.
  bool operator==(const Specificity& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
  }

  /// Gets the 3-tuple of integers.
  const ABC& abc() const { return abc_; }

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

  /// Internal constructor for special values.
  explicit constexpr Specificity(SpecialType special) : special_(special) {}

  ABC abc_;                                  ///< The 3-tuple of integers.
  SpecialType special_ = SpecialType::None;  ///< Special value, if any.
};

}  // namespace donner::css
