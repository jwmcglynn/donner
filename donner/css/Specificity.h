#pragma once
/// @file

#include <compare>
#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::css {

/**
 * A CSS specificity value (id, class, type), as defined in
 * https://www.w3.org/TR/selectors-4/#specificity-rules, which is used during cascading to determine
 * which style takes precedence.
 *
 * The specificity is a 3-tuple of integers, where the first integer is the most significant, plus a
 * few special values such as "!important" which override.
 *
 * The 3-tuple is defined as:
 * - `a`: The number of ID selectors in the selector.
 * - `b`: The number of class selectors, attributes selectors, and pseudo-classes in the selector.
 * - `c`: The number of type selectors and pseudo-elements in the selector.
 *
 * Examples
 * | Selector | Specificity |
 * | ----------- | ----------- |
 * | `#id` | (1, 0, 0) |
 * | `.class` | (0, 1, 0) |
 * | `div` | (0, 0, 1) |
 * | `#id.class` | (1, 1, 0) |
 * | `#id.class div` | (1, 1, 1) |
 * | `#id.class > div` | (1, 1, 1) |
 * | `[class~="class"]` | (0, 1, 0) |
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
  /**
   * Special values for specificity, which take precedence over the 3-tuple.
   *
   * The order of these values is important, since operator<=> considers later enum values to be
   * greater.
   */
  enum class SpecialType : std::uint8_t {
    UserAgent,                ///< Normal user agent stylesheet declaration.
    None,                     ///< Normal author declaration.
    StyleAttribute,           ///< Normal inline style, above normal author selectors.
    Animation,                ///< Declarative animation, below important declarations.
    Important,                ///< Important author selector declaration.
    ImportantStyleAttribute,  ///< Important inline style, above important author selectors.
    ImportantUserAgent,       ///< Important user agent declaration, above author CSS.
    Override                  ///< Explicit C++ values, above every CSS declaration.
  };

  /// Output stream operator.
  friend std::ostream& operator<<(std::ostream& os, const SpecialType& type) {
    switch (type) {
      case SpecialType::UserAgent: return os << "UserAgent";
      case SpecialType::None: return os << "None";
      case SpecialType::StyleAttribute: return os << "StyleAttribute";
      case SpecialType::Animation: return os << "Animation";
      case SpecialType::Important: return os << "Important";
      case SpecialType::ImportantStyleAttribute: return os << "ImportantStyleAttribute";
      case SpecialType::ImportantUserAgent: return os << "ImportantUserAgent";
      case SpecialType::Override: return os << "Override";
    }

    UTILS_UNREACHABLE();
  }

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
   * Specificity(!important, 0, 0, 0)
   * ```
   *
   * @param os Output stream.
   * @param obj Specificity to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const Specificity& obj) {
    os << "Specificity(";
    switch (obj.specialValue()) {
      case SpecialType::StyleAttribute: return os << "style)";
      case SpecialType::Animation: return os << "animation)";
      case SpecialType::ImportantStyleAttribute: return os << "style !important)";
      case SpecialType::Override: return os << "override)";
      case SpecialType::UserAgent: os << "user-agent, "; break;
      case SpecialType::Important: os << "!important, "; break;
      case SpecialType::ImportantUserAgent: os << "user-agent !important, "; break;
      case SpecialType::None: break;
    }
    if (const auto rank = obj.layerRank()) {
      os << "layer " << rank << ", ";
    }
    return os << obj.abc_.a << ", " << obj.abc_.b << ", " << obj.abc_.c << ")";
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

  /// Creates the declarative animation cascade level.
  static constexpr Specificity Animation() { return Specificity(SpecialType::Animation); }

  /**
   * Creates a specificity for an `!important` declaration.
   */
  static constexpr Specificity Important() { return Specificity(SpecialType::Important); }

  /**
   * Marks a declaration important while preserving its origin and selector specificity.
   * Already-important ranks and explicit non-CSS overrides are unchanged.
   *
   * @param specificity Original declaration rank.
   * @return The important rank for \p specificity's origin, with its selector specificity.
   */
  static constexpr Specificity Important(Specificity specificity) {
    switch (specificity.specialValue()) {
      case SpecialType::UserAgent:
        specificity.priority_ =
            EncodePriority(SpecialType::ImportantUserAgent, specificity.layerRank());
        break;
      case SpecialType::None:
        specificity.priority_ = EncodePriority(SpecialType::Important, specificity.layerRank());
        break;
      case SpecialType::StyleAttribute:
        specificity.priority_ =
            EncodePriority(SpecialType::ImportantStyleAttribute, specificity.layerRank());
        break;
      default: break;
    }
    return specificity;
  }

  /**
   * Creates a specificity for a style attribute.
   */
  static constexpr Specificity StyleAttribute() { return Specificity(SpecialType::StyleAttribute); }

  /**
   * Creates a specificity that overrides any other value, for overriding styles from the C++
   * API.
   */
  static constexpr Specificity Override() { return Specificity(SpecialType::Override); }

  /// Assign the document layer's normal-order rank. Rank zero is the unlayered fast path.
  constexpr Specificity withLayerRank(std::uint16_t rank) const {
    Specificity result = *this;
    result.priority_ = EncodePriority(specialValue(), rank);
    return result;
  }

  /// Rank in normal precedence order, before reversal for important origin levels.
  constexpr std::uint16_t layerRank() const {
    return NormalizeLayerOrder(specialValue(), static_cast<std::uint16_t>(priority_ >> 1));
  }

  /// Whether the document supplies a nonzero layer rank for this declaration.
  constexpr bool hasLayerRank() const { return (priority_ & 1u) == 0; }

  /// Spaceship operator.
  auto operator<=>(const Specificity& other) const {
    if (priority_ != other.priority_) {
      return priority_ <=> other.priority_;
    }
    return abc_ <=> other.abc_;
  }

  /// Equality operator, for gtest.
  bool operator==(const Specificity& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
  }

  /// Gets the 3-tuple of integers.
  const ABC& abc() const { return abc_; }

  /// Gets the special value of the specificity, defaults to \ref SpecialType::None.
  constexpr SpecialType specialValue() const { return static_cast<SpecialType>(priority_ >> 17); }

  /// Whether this rank came from an inline style declaration, regardless of importance.
  bool isStyleAttribute() const {
    return specialValue() == SpecialType::StyleAttribute ||
           specialValue() == SpecialType::ImportantStyleAttribute;
  }

  /// Converts a normal selector rank to user-agent origin; special ranks are unchanged.
  Specificity toUserAgentSpecificity() const {
    Specificity result = *this;
    if (result.specialValue() == SpecialType::None) {
      result.priority_ = EncodePriority(SpecialType::UserAgent, result.layerRank());
    }

    return result;
  }

private:
  static constexpr std::uint16_t NormalizeLayerOrder(SpecialType origin, std::uint16_t rank) {
    const bool reversesOrder =
        origin == SpecialType::Important || origin == SpecialType::ImportantUserAgent;
    return reversesOrder ? static_cast<std::uint16_t>(0xffffu - rank) : rank;
  }

  static constexpr std::uint32_t EncodePriority(SpecialType origin, std::uint16_t layerRank) {
    // The low bit gives inheritance a cheap zero-rank check. It cannot affect precedence
    // because it differs only when the normalized 16-bit rank already differs.
    return (static_cast<std::uint32_t>(origin) << 17) |
           (static_cast<std::uint32_t>(NormalizeLayerOrder(origin, layerRank)) << 1) |
           static_cast<std::uint32_t>(layerRank == 0);
  }

  /// Internal constructor for special values.
  explicit constexpr Specificity(SpecialType special) : priority_(EncodePriority(special, 0)) {}

  ABC abc_;  ///< The 3-tuple of integers.
  /// The ordered origin occupies the higher bits, with precedence-normalized layer rank below.
  std::uint32_t priority_ = EncodePriority(SpecialType::None, 0);
};

}  // namespace donner::css
