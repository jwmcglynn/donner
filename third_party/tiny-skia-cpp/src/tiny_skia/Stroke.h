#pragma once

/// @file Stroke.h
/// @brief Stroke properties and dash patterns.

#include <cstddef>
#include <cstdint>
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

/// Bounds the outline a stroke expansion may emit. Not in upstream tiny-skia: Donner renders
/// untrusted documents, and under an extreme transform the stroker can emit millions of points
/// for one curve. A stroke whose outline passes \ref maxPoints stops expanding as soon as it does,
/// produces no outline, and sets \ref exceeded. The finished outline is checked exactly, so no
/// outline over \ref maxPoints is returned. A limit whose \ref exceeded is already set produces no
/// outline at all.
struct StrokeOutputLimit {
  std::size_t maxPoints = 0;      ///< Most outline points the expansion may emit.
  bool exceeded = false;          ///< Set when an expansion stopped at \ref maxPoints.
  std::size_t emittedPoints = 0;  ///< Outline points the expansion emitted, set by the stroker.
};

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
