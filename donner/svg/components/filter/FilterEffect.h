#pragma once
/// @file

#include <variant>

#include "donner/base/Length.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg {

/**
 * Filter effect container, which can contain a reference to another filter effect, or a filter
 * effect itself (of any type).
 */
struct FilterEffect {
  /// No effect.
  struct None {
    /// Equality operator.
    bool operator==(const None&) const = default;
  };

  /// Blur effect, which applies a gaussian blur with the given standard deviation.

  struct Blur {
    /// X-component of the standard deviation of the blur, in pixels.
    Lengthd stdDeviationX;
    /// Y-component of the standard deviation of the blur, in pixels.
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

  /**
   * Variant containing all supported effects.
   */
  using Type = std::variant<None, Blur, ElementReference>;

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
   * Returns the filter effect as type \c T, asserting that the filter effect is of type \ref T.
   *
   * @tparam T Type to get.
   */
  template <typename T>
  T& get() & {
    return std::get<T>(value);
  }

  /**
   * Returns the filter effect as type \c T, asserting that the filter effect is of type \ref T.
   *
   * @tparam T Type to get.
   */
  template <typename T>
  const T& get() const& {
    return std::get<T>(value);
  }

  /**
   * Returns the filter effect as type \c T, asserting that the filter effect is of type \ref T.
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

}  // namespace donner::svg
