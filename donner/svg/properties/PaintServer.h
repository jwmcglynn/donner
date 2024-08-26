#pragma once
/// @file

#include <cassert>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/css/Color.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg {

/**
 * Represents a paint server, which can be a solid color, a reference to another element, or a
 * special value like "none" or "context-fill".
 */
struct PaintServer {
  /// Represents the "none" value for a paint server.
  struct None {
    /// Equality operator.
    bool operator==(const None&) const = default;
  };

  /// Represents the "context-fill" value for a paint server.
  struct ContextFill {
    /// Equality operator.
    bool operator==(const ContextFill&) const = default;
  };

  /// Represents the "context-stroke" value for a paint server.
  struct ContextStroke {
    /// Equality operator.
    bool operator==(const ContextStroke&) const = default;
  };

  /// Represents a solid color paint server.
  struct Solid {
    /// The color of the paint server.
    css::Color color;

    /// Construct a solid color paint server with the given color.
    constexpr explicit Solid(css::Color color) : color(color) {}

    /// Equality operator.
    bool operator==(const Solid&) const = default;
  };

  /// Represents a reference to another element, which originates from a `url()` reference. Should
  /// point to another paint server.
  struct ElementReference {
    /// The reference to the other element.
    Reference reference;
    /// A fallback color which is used if the referenced element is not found. If not specified, the
    /// paint will fallback to \ref None.
    std::optional<css::Color> fallback;

    /// Construct a reference to another element with the given reference and fallback color.
    ElementReference(Reference reference, std::optional<css::Color> fallback = std::nullopt)
        : reference(std::move(reference)), fallback(fallback) {}

    /// Equality operator.
    bool operator==(const ElementReference&) const = default;
  };

  /// Variant which can hold valid paint server types.
  using Type = std::variant<None, ContextFill, ContextStroke, Solid, ElementReference>;

  /// Current paint server.
  Type value;

  /* implicit */ PaintServer(Type value) : value(std::move(value)) {}

  // Allow constexpr construction for None and Solid.
  /// Construct a paint server with no value, \ref None.
  /* implicit */ constexpr PaintServer(None) : value(None{}) {}
  /// Construct a paint server for a solid color.
  /* implicit */ constexpr PaintServer(Solid solid) : value(solid) {}

  /// Equality operator.
  bool operator==(const PaintServer& other) const;

  /**
   * Returns true if the paint server is of the requested type.
   *
   * @tparam T The type of the paint server to check.
   */
  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(value);
  }

  /**
   * Returns the value of the paint server, if it is of the requested type.
   *
   * @tparam T The type of the paint server to get.
   */
  template <typename T>
  T& get() & {
    return std::get<T>(value);
  }

  /**
   * Returns the value of the paint server, if it is of the requested type.
   *
   * @tparam T The type of the paint server to get.
   */
  template <typename T>
  const T& get() const& {
    return std::get<T>(value);
  }

  /**
   * Returns the value of the paint server, if it is of the requested type.
   *
   * @tparam T The type of the paint server to get.
   */
  template <typename T>
  T&& get() && {
    return std::move(std::get<T>(value));
  }

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const PaintServer& paint);
};

}  // namespace donner::svg
