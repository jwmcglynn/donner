#pragma once
/// @file

#include <cstdint>
#include <vector>

#include "donner/backends/tiny_skia_cpp/AlphaRuns.h"
#include "donner/backends/tiny_skia_cpp/Mask.h"
#include "donner/backends/tiny_skia_cpp/PathGeometry.h"
#include "donner/backends/tiny_skia_cpp/Transform.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::backends::tiny_skia_cpp {

/** Edge segment used for scan conversion. */
struct EdgeSegment {
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  double slope = 0.0;
  int32_t firstY = 0;
  int32_t lastY = -1;
  int8_t winding = 0;

  /// Returns true when the edge intersects the integer scanline at \a y.
  bool coversScanline(int32_t y) const { return y >= firstY && y <= lastY; }

  /// Computes the x intersection with the scanline centered at y + 0.5.
  double xAtScanline(int32_t y) const {
    return x0 + slope * ((static_cast<double>(y) + 0.5) - y0);
  }
};

/**
 * Converts a PathSpline into monotonic edge segments suitable for rasterization.
 */
std::vector<EdgeSegment> BuildEdges(const svg::PathSpline& spline, const Transform& transform);

/**
 * Rasterizes a filled PathSpline into an 8-bit coverage mask.
 */
Mask RasterizeFill(const svg::PathSpline& spline, int width, int height,
                   FillRule fillRule = FillRule::kNonZero, bool antiAlias = true,
                   const Transform& transform = Transform());

}  // namespace donner::backends::tiny_skia_cpp

