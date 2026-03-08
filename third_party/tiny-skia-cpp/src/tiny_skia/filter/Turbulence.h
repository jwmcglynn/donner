#pragma once

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/// Noise type for feTurbulence.
enum class TurbulenceType {
  FractalNoise,  ///< Fractal Brownian motion (signed noise summed).
  Turbulence,    ///< Turbulence (absolute value of noise summed).
};

/// Parameters for the SVG feTurbulence filter primitive.
struct TurbulenceParams {
  TurbulenceType type = TurbulenceType::Turbulence;
  double baseFrequencyX = 0.0;
  double baseFrequencyY = 0.0;
  int numOctaves = 1;
  double seed = 0.0;
  bool stitchTiles = false;
  int tileWidth = 0;   ///< Tile width for stitching (in user-space pixels).
  int tileHeight = 0;  ///< Tile height for stitching (in user-space pixels).

  // Coordinate transform from pixel space to user space.
  // User-space coordinate = pixel_coordinate / scale.
  double scaleX = 1.0;  ///< Horizontal scale (canvas pixels per user-space unit).
  double scaleY = 1.0;  ///< Vertical scale (canvas pixels per user-space unit).
};

/**
 * Generate Perlin turbulence noise into \p dst.
 *
 * Implements the SVG feTurbulence algorithm as defined in the Filter Effects spec.
 * The noise is generated in the coordinate space of the destination pixmap.
 *
 * @param dst Destination pixmap to fill with noise.
 * @param params Turbulence parameters.
 */
void turbulence(Pixmap& dst, const TurbulenceParams& params);

/// Float-precision version of turbulence.
void turbulence(FloatPixmap& dst, const TurbulenceParams& params);

}  // namespace tiny_skia::filter
