#pragma once
/// @file

#include <cstddef>
#include <ostream>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/Length.h"
#include "donner/base/ParseResult.h"
#include "donner/css/ComponentValue.h"
#include "donner/svg/core/ClipRule.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg {

/// Shared reference box and center for radial CSS clipping shapes.
struct RadialClipPath {
  /// Reference geometry for the shape's size and position.
  enum class Box { Fill, Stroke, View };
  /// Radius interpretation: explicit length/percentage or distance to a reference-box side.
  enum class Radius { Length, ClosestSide, FarthestSide };
  /// One position coordinate, measured from the near or far edge of its reference box.
  struct Position {
    Lengthd offset{50.0, Lengthd::Unit::Percent};
    bool fromFarEdge = false;  ///< Interpret the offset from the right or bottom edge.
    bool operator==(const Position&) const = default;
  };

  Box box = Box::Stroke;  ///< SVG's default border-box maps to stroke-box.
  Position x;             ///< Horizontal center position.
  Position y;             ///< Vertical center position.
  bool operator==(const RadialClipPath&) const = default;
};

/// A circle used as a CSS clipping shape.
struct CircleClipPath : RadialClipPath {
  Radius radiusType = Radius::ClosestSide;  ///< Default radius is the nearest side.
  Lengthd radius;                           ///< Explicit radius, when selected.
  bool operator==(const CircleClipPath&) const = default;
};

/// An ellipse used as a CSS clipping shape, with independently resolved axis radii.
struct EllipseClipPath : RadialClipPath {
  Radius radiusXType = Radius::ClosestSide;  ///< Horizontal radius interpretation.
  Radius radiusYType = Radius::ClosestSide;  ///< Vertical radius interpretation.
  Lengthd radiusX;                           ///< Explicit horizontal radius.
  Lengthd radiusY;                           ///< Explicit vertical radius.
  bool operator==(const EllipseClipPath&) const = default;
};

/// A polygon used as a CSS clipping shape.
struct PolygonClipPath {
  /// Maximum accepted vertices, matching the SVG point-list resource limit.
  static constexpr std::size_t kMaximumVertices = 16 * 1024;

  /// A vertex relative to the reference box's top-left corner.
  struct Vertex {
    Lengthd x;  ///< Horizontal offset.
    Lengthd y;  ///< Vertical offset.
    bool operator==(const Vertex&) const = default;
  };

  RadialClipPath::Box box = RadialClipPath::Box::Stroke;  ///< SVG reference geometry.
  ClipRule rule = ClipRule::NonZero;                      ///< Polygon interior rule.
  std::vector<Vertex> vertices;                           ///< Bounded, owning vertex list.
  bool operator==(const PolygonClipPath&) const = default;
};

/// A referenced SVG clip or a supported CSS basic shape; absence is represented by the property.
struct ClipPath {
  std::variant<Reference, CircleClipPath, EllipseClipPath, PolygonClipPath>
      value;  ///< Owning parsed value.
  explicit ClipPath(Reference reference) : value(std::move(reference)) {}
  explicit ClipPath(CircleClipPath circle) : value(std::move(circle)) {}
  explicit ClipPath(EllipseClipPath ellipse) : value(std::move(ellipse)) {}
  explicit ClipPath(PolygonClipPath polygon) : value(std::move(polygon)) {}

  /// Return the basic shape's interior rule; references provide their own rules.
  ClipRule clipRule() const {
    const auto* polygon = std::get_if<PolygonClipPath>(&value);
    return polygon ? polygon->rule : ClipRule::NonZero;
  }

  bool operator==(const ClipPath&) const = default;
};

/// Print a parsed clip-path value for diagnostics.
std::ostream& operator<<(std::ostream& os, const ClipPath& clip);

/// Parse a supported basic clipping shape and its optional reference box.
/// @param components Trimmed CSS component values.
/// @param allowUserUnits Permit SVG presentation-attribute user units.
ParseResult<ClipPath> ParseBasicShapeClipPath(std::span<const css::ComponentValue> components,
                                              bool allowUserUnits);

}  // namespace donner::svg
