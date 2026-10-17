#pragma once
/// @file
/// Backend-neutral resolution of Donner paint servers into solid colors and gradient
/// descriptions, so every third-party adapter interprets SVG paint identically.

#include <cstdint>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/svg/components/RenderingInstanceComponent.h"

namespace donner::bakeoff {

/// Straight-alpha color with channels in [0, 1].
struct ColorF {
  float r = 0.0f;  //!< Red.
  float g = 0.0f;  //!< Green.
  float b = 0.0f;  //!< Blue.
  float a = 0.0f;  //!< Alpha.

  /// Converts a resolved 8-bit color.
  /// @param rgba Color to convert.
  static ColorF FromRgba(const css::RGBA& rgba);

  /// Returns the color as 8-bit straight-alpha channels.
  [[nodiscard]] css::RGBA toRgba() const;
};

/// Gradient spread behavior outside [0, 1].
enum class SpreadMode : uint8_t {
  Pad,      //!< Clamp to the end stops.
  Reflect,  //!< Mirror the ramp.
  Repeat,   //!< Repeat the ramp.
};

/// One gradient stop with opacity already applied.
struct GradientStopF {
  float offset = 0.0f;  //!< Offset in [0, 1], non-decreasing across the stop list.
  ColorF color;         //!< Straight-alpha color.
};

/// A linear or radial gradient in the coordinate space of the path being painted.
struct GradientDesc {
  /// Gradient geometry family.
  enum class Type : uint8_t {
    Linear,  //!< Uses \ref start and \ref end.
    Radial,  //!< Uses \ref center, \ref radius, \ref focal and \ref focalRadius.
  };

  Type type = Type::Linear;             //!< Geometry family.
  Vector2d start = Vector2d::Zero();    //!< Linear start, in gradient space.
  Vector2d end = Vector2d::Zero();      //!< Linear end, in gradient space.
  Vector2d center = Vector2d::Zero();   //!< Radial end-circle center, in gradient space.
  double radius = 0.0;                  //!< Radial end-circle radius.
  Vector2d focal = Vector2d::Zero();    //!< Radial start-circle center, in gradient space.
  double focalRadius = 0.0;             //!< Radial start-circle radius.
  SpreadMode spread = SpreadMode::Pad;  //!< Spread mode.
  Transform2d pathFromGradient;         //!< Maps gradient space into path-local space.
  std::vector<GradientStopF> stops;     //!< At least two stops.
};

/// A resolved paint for one fill or stroke pass.
struct PaintDesc {
  /// Resolved paint family.
  enum class Kind : uint8_t {
    None,      //!< Nothing to draw.
    Solid,     //!< Draw \ref color.
    Gradient,  //!< Draw \ref gradient.
    /// Draw the pattern tile most recently recorded through `endPatternTile`. Produced only for
    /// adapters that accepted that tile.
    Pattern,
  };

  Kind kind = Kind::None;  //!< Paint family.
  ColorF color;            //!< Solid color with opacity applied, for \ref Kind::Solid.
  GradientDesc gradient;   //!< Gradient, for \ref Kind::Gradient.
};

/**
 * Resolves a fill or stroke paint server the way Donner's software renderer does: solid colors
 * resolve `currentColor` and opacity, gradients resolve units, transforms and stops, a gradient
 * with one stop becomes solid, and a gradient that cannot be instantiated uses its fallback color
 * or draws nothing.
 *
 * @param paint Resolved paint server from the driver.
 * @param opacity Fill or stroke opacity.
 * @param currentColor Value of `currentColor`.
 * @param pathBounds Local-space bounds of the geometry being painted, for objectBoundingBox units.
 * @param viewBox View box used for userSpaceOnUse units.
 */
PaintDesc ResolvePaint(const svg::components::ResolvedPaintServer& paint, double opacity,
                       const css::RGBA& currentColor, const Box2d& pathBounds,
                       const Box2d& viewBox);

}  // namespace donner::bakeoff
