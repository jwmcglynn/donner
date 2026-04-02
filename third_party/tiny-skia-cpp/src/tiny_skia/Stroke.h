#pragma once

/// @file Stroke.h
/// @brief Stroke properties and dash patterns.

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
};

/// Stroke properties for Painter::strokePath.
struct Stroke {
  float width = 1.0f;         ///< Stroke width in pixels.
  float miterLimit = 4.0f;    ///< Miter join limit (ratio of miter length to stroke width).
  LineCap lineCap = LineCap::Butt;     ///< Endpoint cap style.
  LineJoin lineJoin = LineJoin::Miter;  ///< Corner join style.
  std::optional<StrokeDash> dash;       ///< Optional dash pattern.
};

}  // namespace tiny_skia
