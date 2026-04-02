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

  // Inverse transform from pixel (device) space to filter (user) space.
  // User-space coordinate = filterFromDevice * pixel_coordinate.
  // This is a 2x2 matrix stored as [a, b, c, d] where:
  //   ux = a * px + b * py
  //   uy = c * px + d * py
  double filterFromDeviceA = 1.0;  ///< [0,0] element
  double filterFromDeviceB = 0.0;  ///< [0,1] element
  double filterFromDeviceC = 0.0;  ///< [1,0] element
  double filterFromDeviceD = 1.0;  ///< [1,1] element
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
