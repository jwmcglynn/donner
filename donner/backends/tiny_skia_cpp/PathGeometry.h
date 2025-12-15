#pragma once
/// @file

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "donner/backends/tiny_skia_cpp/Stroke.h"
#include "donner/base/Box.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::backends::tiny_skia_cpp {

/** Lightweight point type used for tiny-skia geometry. */
struct PathPoint {
  float x;
  float y;
};

/** Commands emitted when iterating a PathSpline. */
enum class PathVerb : uint8_t {
  kMove,
  kLine,
  kCubic,
  kClose,
};

/** Segment describing the current path verb and its control points. */
struct PathSegment {
  PathVerb verb;
  std::array<PathPoint, 3> points{};
  size_t pointCount = 0;
  bool isInternalPoint = false;
};

/** Fill winding rules for rasterization. */
enum class FillRule { kNonZero, kEvenOdd };

/**
 * Iterates a donner::svg::PathSpline and produces tiny-skia style segments.
 *
 * This adapter lets the C++ backend consume the existing PathSpline data
 * without defining a parallel path container.
 */
class PathIterator {
public:
  explicit PathIterator(const svg::PathSpline& spline);

  /// Resets iteration to the first command.
  void reset();

  /**
   * Advances iteration and returns the next segment if available.
   *
   * @return Next PathSegment, or std::nullopt when iteration is finished.
   */
  std::optional<PathSegment> next();

private:
  PathSegment buildSegment(const svg::PathSpline::Command& command) const;

  const svg::PathSpline* spline_ = nullptr;
  size_t commandIndex_ = 0;
};

/**
 * Computes the tight axis-aligned bounding box for a PathSpline.
 *
 * This mirrors tiny-skia's behavior by accounting for cubic Bézier extrema
 * rather than only using end points. Empty paths return std::nullopt.
 */
std::optional<Boxd> ComputeBoundingBox(const svg::PathSpline& spline);

/**
 * Applies a dash pattern to a PathSpline, returning a dashed path.
 *
 * Curves are flattened to line segments using a small tolerance to align with
 * tiny-skia's dash builder behavior.
 */
svg::PathSpline ApplyDash(const svg::PathSpline& spline, const StrokeDash& dash);

/**
 * Builds an outline polygon for the stroked path.
 *
 * Applies optional dash patterns before constructing the stroke geometry. The
 * resulting spline is ready for filling.
 */
svg::PathSpline ApplyStroke(const svg::PathSpline& spline, const Stroke& stroke);

/**
 * Computes the bounding box of a stroked path, including caps and joins.
 *
 * Dash patterns are applied before stroking to mirror rendering behavior. An
 * empty path returns std::nullopt.
 */
std::optional<Boxd> ComputeStrokeBounds(const svg::PathSpline& spline, const Stroke& stroke);

}  // namespace donner::backends::tiny_skia_cpp
