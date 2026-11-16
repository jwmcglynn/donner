#pragma once

/// @file Stroke.h
/// @brief Stroke properties and dash patterns.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "tiny_skia/Path.h"
#include "tiny_skia/Scalar.h"

namespace tiny_skia {

/// Line join style for stroke corners.
enum class LineJoin : std::uint8_t {
  Miter,     ///< Sharp corner (extends to miter limit).
  MiterClip, ///< Miter clipped at the limit distance.
  Round,     ///< Rounded corner.
  Bevel,     ///< Flat corner.
};

/// Dash pattern for stroked paths.
struct StrokeDash {
  /// Dash/gap lengths. Must have an even count with all values > 0.
  std::vector<float> array;
  /// Offset into the dash pattern.
  float offset = 0.0f;

  /// Creates a validated dash. Returns nullopt if the array is invalid.
  [[nodiscard]] static std::optional<StrokeDash> create(std::vector<float> dashArray,
                                                        float dashOffset);

  friend bool operator==(const StrokeDash&, const StrokeDash&) = default;
};

/// Bounds the geometry generating one stroke may take: the curve measurement its dash pattern
/// performs and the outline its expansion emits. Not in upstream tiny-skia: Donner renders
/// untrusted documents, and under an extreme transform one curve can take millions of measurement
/// steps or outline points.
///
/// The counts are totals over every dash and stroke run under the limit. A dash or stroke whose
/// count passes its bound stops as soon as it does, produces no path, and sets \ref exceeded; the
/// finished outline is checked exactly, so no outline that passes \ref maxOutlinePoints is
/// returned. Once \ref exceeded is set, nothing more is dashed or stroked under the limit.
struct StrokeLimits {
  /// Most curve-measurement steps dashing may take: every subdivision step of a curve, and every
  /// line, counts as one, whether or not it adds a measured segment.
  std::size_t maxMeasureSteps = std::numeric_limits<std::size_t>::max();
  /// Most outline points stroke expansion may emit.
  std::size_t maxOutlinePoints = std::numeric_limits<std::size_t>::max();
  std::size_t measureSteps = 0;   ///< Curve-measurement steps taken, counted by the dasher.
  std::size_t outlinePoints = 0;  ///< Outline points emitted, counted by the stroker.
  bool exceeded = false;          ///< Set when a dash or stroke stopped at its bound.
};

/// Lowers \p resScale, if need be, so that the curve tolerance `toleranceAtUnitScale / resScale`
/// a stroke or dash derives from it stays at least one float step of the largest coordinate it
/// works with, \p maxCoordinate. Not in upstream tiny-skia: below that precision a smaller
/// tolerance changes no output point, while curve subdivision chases rounding error until its
/// depth limits. The scale is lowered only above `toleranceAtUnitScale / step(maxCoordinate)`;
/// there the tolerance stays one float step, an error of about `step(maxCoordinate) * resScale`
/// device pixels.
[[nodiscard]] float PrecisionLimitedResScale(float resScale, float maxCoordinate,
                                             float toleranceAtUnitScale);

/// The largest absolute coordinate of \p path's control points, grown by \p reach.
[[nodiscard]] float MaxAbsCoordinate(const Path& path, float reach);

/// Stroke properties for Painter::strokePath.
struct Stroke {
  float width = 1.0f;         ///< Stroke width in pixels.
  float miterLimit = 4.0f;    ///< Miter join limit (ratio of miter length to stroke width).
  LineCap lineCap = LineCap::Butt;     ///< Endpoint cap style.
  LineJoin lineJoin = LineJoin::Miter;  ///< Corner join style.
  std::optional<StrokeDash> dash;       ///< Optional dash pattern.

  /// Two strokes compare equal when they expand an outline the same way, which is what lets a
  /// caller keep work derived from a stroke and decide later whether it still applies. Floats
  /// compare with IEEE semantics: a NaN width is never equal even to itself, so such a stroke
  /// is never reused, and positive and negative zero compare equal, which the stroker cannot
  /// distinguish either.
  friend bool operator==(const Stroke&, const Stroke&) = default;
};

}  // namespace tiny_skia
