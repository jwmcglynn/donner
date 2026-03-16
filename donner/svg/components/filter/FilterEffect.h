#pragma once
/// @file

#include <variant>

#include "donner/base/Length.h"
#include "donner/css/Color.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg {

/**
 * Filter effect container, which can contain a reference to another filter effect, or a CSS filter
 * function (such as `blur()`, `grayscale()`, etc).
 *
 * @see https://www.w3.org/TR/filter-effects/#FilterProperty
 */
struct FilterEffect {
  /// No effect.
  struct None {
    /// Equality operator.
    bool operator==(const None&) const = default;
  };

  /// Blur effect, which applies a gaussian blur with the given standard deviation.
  struct Blur {
    /// X-component of the standard deviation of the blur.
    Lengthd stdDeviationX;
    /// Y-component of the standard deviation of the blur.
    Lengthd stdDeviationY;

    /// Equality operator.
    bool operator==(const Blur&) const = default;
  };

  /// Reference to another filter effect, from a `url()`
  struct ElementReference {
    Reference reference;  ///< Reference to another filter effect.

    /// Constructor.
    explicit ElementReference(Reference reference) : reference(std::move(reference)) {}

    /// Equality operator.
    bool operator==(const ElementReference&) const = default;
  };

  /// CSS `hue-rotate(<angle>)` filter function.
  struct HueRotate {
    double angleDegrees = 0.0;  ///< Rotation angle in degrees.

    /// Equality operator.
    bool operator==(const HueRotate&) const = default;
  };

  /// CSS `brightness(<number-percentage>)` filter function.
  struct Brightness {
    double amount = 1.0;  ///< Multiplier (1.0 = no change).

    /// Equality operator.
    bool operator==(const Brightness&) const = default;
  };

  /// CSS `contrast(<number-percentage>)` filter function.
  struct Contrast {
    double amount = 1.0;  ///< Multiplier (1.0 = no change).

    /// Equality operator.
    bool operator==(const Contrast&) const = default;
  };

  /// CSS `grayscale(<number-percentage>)` filter function.
  struct Grayscale {
    double amount = 1.0;  ///< Amount (0.0 = no change, 1.0 = fully grayscale).

    /// Equality operator.
    bool operator==(const Grayscale&) const = default;
  };

  /// CSS `invert(<number-percentage>)` filter function.
  struct Invert {
    double amount = 1.0;  ///< Amount (0.0 = no change, 1.0 = fully inverted).

    /// Equality operator.
    bool operator==(const Invert&) const = default;
  };

  /// CSS `opacity(<number-percentage>)` filter function.
  struct FilterOpacity {
    double amount = 1.0;  ///< Amount (1.0 = fully opaque, 0.0 = transparent).

    /// Equality operator.
    bool operator==(const FilterOpacity&) const = default;
  };

  /// CSS `saturate(<number-percentage>)` filter function.
  struct Saturate {
    double amount = 1.0;  ///< Multiplier (1.0 = no change).

    /// Equality operator.
    bool operator==(const Saturate&) const = default;
  };

  /// CSS `sepia(<number-percentage>)` filter function.
  struct Sepia {
    double amount = 1.0;  ///< Amount (0.0 = no change, 1.0 = fully sepia).

    /// Equality operator.
    bool operator==(const Sepia&) const = default;
  };

  /// CSS `drop-shadow()` filter function.
  struct DropShadow {
    Lengthd offsetX{0.0, Lengthd::Unit::Px};       ///< Horizontal offset.
    Lengthd offsetY{0.0, Lengthd::Unit::Px};       ///< Vertical offset.
    Lengthd stdDeviation{0.0, Lengthd::Unit::Px};  ///< Blur standard deviation.
    css::Color color{css::RGBA(0, 0, 0, 0xFF)};    ///< Shadow color (default: black).

    /// Equality operator.
    bool operator==(const DropShadow&) const = default;
  };

  /**
   * Variant containing all supported effects.
   */
  using Type = std::variant<None, Blur, ElementReference, HueRotate, Brightness, Contrast,
                            Grayscale, Invert, FilterOpacity, Saturate, Sepia, DropShadow>;

  /// Filter effect variant value, contains the current effect.
  Type value;

  /**
   * Construct a filter effect from a pre-constructed filter.
   *
   * @param value Filter effect to assign.
   */
  /* implicit */ FilterEffect(Type value) : value(std::move(value)) {}

  /**
   * Construct an empty filter, which applies no effect, as a constexpr constructor.
   */
  /* implicit */ constexpr FilterEffect(None) : value(None{}) {}

  /// Equality operator.
  bool operator==(const FilterEffect& other) const;

  /**
   * Returns true if this filter effect is of type \c T.
   *
   * @tparam T Type to check.
   */
  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(value);
  }

  /**
   * Returns the filter effect as type \c T, asserting that the filter effect is of type \c T.
   *
   * @tparam T Type to get.
   */
  template <typename T>
  T& get() & {
    return std::get<T>(value);
  }

  /**
   * Returns the filter effect as type \c T, asserting that the filter effect is of type \c T.
   *
   * @tparam T Type to get.
   */
  template <typename T>
  const T& get() const& {
    return std::get<T>(value);
  }

  /**
   * Returns the filter effect as type \c T, asserting that the filter effect is of type \c T.
   *
   * @tparam T Type to get.
   */
  template <typename T>
  T&& get() && {
    return std::move(std::get<T>(value));
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const FilterEffect& filter);
};

/// Ostream output operator for a vector of filter effects.
std::ostream& operator<<(std::ostream& os, const std::vector<FilterEffect>& filters);

}  // namespace donner::svg
