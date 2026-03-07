#pragma once

#include <cstdint>

#include "tiny_skia/FixedPoint.h"
#include "tiny_skia/Point.h"

namespace tiny_skia {

/// Analytic edge for Analytic Anti-Aliasing (AAA), ported from Skia's SkAnalyticEdge.
///
/// Unlike the super-sampling approach, analytic edges store sub-pixel Y coordinates
/// in 16.16 fixed-point format. The edge walker steps at non-uniform Y intervals
/// (edge endpoints, pixel boundaries, intersections) and computes exact geometric
/// coverage per pixel using trapezoid area formulas.
struct AnalyticEdge {
  AnalyticEdge* prev = nullptr;
  AnalyticEdge* next = nullptr;

  FDot16 x = 0;       ///< Current X position (16.16 fixed-point).
  FDot16 dx = 0;      ///< Slope dX/dY (16.16 fixed-point).
  FDot16 upperX = 0;  ///< X at upperY.
  FDot16 y = 0;       ///< Current Y (16.16 fixed-point).
  FDot16 upperY = 0;  ///< Upper Y bound (16.16 fixed-point).
  FDot16 lowerY = 0;  ///< Lower Y bound (16.16 fixed-point).
  FDot16 dy = 0;      ///< abs(1/dx) for trapezoid blitting (16.16 fixed-point).

  std::int8_t curveCount = 0;   ///< >0 for quad, <0 for cubic, 0 for line.
  std::uint8_t curveShift = 0;  ///< Subdivision precision shift.
  std::int8_t winding = 0;      ///< +1 (CW) or -1 (CCW).

  // Quadratic fields (used when curveCount > 0).
  FDot16 qx = 0, qy = 0;
  FDot16 qdx = 0, qdy = 0;
  FDot16 qddx = 0, qddy = 0;
  FDot16 qLastX = 0, qLastY = 0;
  FDot16 snappedX = 0, snappedY = 0;

  // Cubic fields (used when curveCount < 0).
  FDot16 cx = 0, cy = 0;
  FDot16 cdx = 0, cdy = 0;
  FDot16 cddx = 0, cddy = 0;
  FDot16 cdddx = 0, cdddy = 0;
  FDot16 cLastX = 0, cLastY = 0;
  std::uint8_t cubicDShift = 0;

  /// Snapping accuracy: quarter-pixel (1 << (16-2) = 16384).
  static constexpr int kDefaultAccuracy = 2;

  /// Snap Y to quarter-pixel boundary.
  static FDot16 snapY(FDot16 y);

  /// Advance edge to target Y position.
  void goY(FDot16 targetY);

  /// Advance edge by a fractional Y step (used by the edge walker).
  void goY(FDot16 targetY, int yShift);

  /// Initialize as a line edge from p0 to p1.
  bool setLine(Point p0, Point p1);

  /// Update the line segment for curved edges (called during curve iteration).
  bool updateLine(FDot16 x0, FDot16 y0, FDot16 x1, FDot16 y1, FDot16 slope);

  /// Advance to the next curve segment. Returns true if the edge is still active.
  bool update(FDot16 lastY);

  /// Initialize as a quadratic edge.
  bool setQuadratic(const Point pts[3]);

  /// Advance to the next quadratic segment.
  bool updateQuadratic();

  /// Maintain X continuity before updating a quadratic edge.
  void keepContinuousQuad();

  /// Initialize as a cubic edge.
  bool setCubic(const Point pts[4]);

  /// Advance to the next cubic segment.
  bool updateCubic();

  /// Maintain X continuity before updating a cubic edge.
  void keepContinuousCubic();
};

/// Quick division using Skia's inverse lookup table for bit-exact results.
FDot16 quickDiv(FDot6 a, FDot6 b);

}  // namespace tiny_skia
