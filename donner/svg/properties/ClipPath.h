#pragma once
/// @file

#include <ostream>
#include <span>
#include <utility>
#include <variant>

#include "donner/base/Length.h"
#include "donner/base/ParseResult.h"
#include "donner/css/ComponentValue.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg {

/// A circle used as a CSS clipping shape.
struct CircleClipPath {
  /// Reference geometry for the circle's size and position.
  enum class Box { Fill, Stroke, View };
  /// Radius interpretation; percentages use the reference box's normalized diagonal.
  enum class Radius { Length, ClosestSide, FarthestSide };
  /// One position coordinate, measured from the near or far edge of its reference box.
  struct Position {
    Lengthd offset{50.0, Lengthd::Unit::Percent};
    bool fromFarEdge = false;  ///< Interpret the offset from the right or bottom edge.
    bool operator==(const Position&) const = default;
  };

  Box box = Box::Stroke;                    ///< SVG's default border-box maps to stroke-box.
  Radius radiusType = Radius::ClosestSide;  ///< Default radius is the nearest side.
  Lengthd radius;                           ///< Explicit radius, when selected.
  Position x;                               ///< Horizontal center position.
  Position y;                               ///< Vertical center position.
  bool operator==(const CircleClipPath&) const = default;
};

/// A referenced SVG clip or a supported CSS basic shape; absence is represented by the property.
struct ClipPath {
  std::variant<Reference, CircleClipPath> value;  ///< Owning parsed value.
  explicit ClipPath(Reference reference) : value(std::move(reference)) {}
  explicit ClipPath(CircleClipPath circle) : value(std::move(circle)) {}
  bool operator==(const ClipPath&) const = default;
};

/// Print a parsed clip-path value for diagnostics.
std::ostream& operator<<(std::ostream& os, const ClipPath& clip);

/// Parse a circle clipping shape and its optional reference box.
/// @param components Trimmed CSS component values.
/// @param allowUserUnits Permit SVG presentation-attribute user units.
ParseResult<ClipPath> ParseCircleClipPath(std::span<const css::ComponentValue> components,
                                          bool allowUserUnits);

}  // namespace donner::svg
