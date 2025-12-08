#pragma once
/// @file

#include <vector>

#include "donner/base/Vector2.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::svg {

/**
 * Portion of a PathSpline command expressed as a curve span with an explicit parameter range.
 */
struct PathCurveSpan {
  PathSpline::CommandType type;  ///< Command type that produced this span.
  size_t commandIndex;           ///< Index into PathSpline::commands().
  double startT;                 ///< Start parameter within the source command, in [0, 1].
  double endT;                   ///< End parameter within the source command, in [0, 1].

  Vector2d startPoint;  ///< Span start point in absolute coordinates.
  Vector2d endPoint;    ///< Span end point in absolute coordinates.

  /// Control points for cubic spans. Undefined for line spans.
  Vector2d controlPoint1;
  Vector2d controlPoint2;

  [[nodiscard]] bool isCubic() const { return type == PathSpline::CommandType::CurveTo; }
  [[nodiscard]] bool isLine() const {
    return type == PathSpline::CommandType::LineTo || type == PathSpline::CommandType::ClosePath;
  }
};

/**
 * Closed or open subpath produced from a PathSpline MoveTo segment.
 */
struct PathSubpathView {
  Vector2d moveTo;                   ///< Starting point of the subpath.
  std::vector<PathCurveSpan> spans;  ///< Curve spans in drawing order.
  bool closed = false;               ///< True if a ClosePath was encountered.
};

/**
 * Segmented view of a PathSpline ready for Boolean processing. Each span maps back to the
 * originating command and preserves curve primitives wherever possible.
 */
struct SegmentedPath {
  std::vector<PathSubpathView> subpaths;  ///< All subpaths with explicit closure spans.
};

/**
 * Default tolerance for segmenting highly curved spans while keeping curve primitives intact.
 */
inline constexpr double kDefaultSegmentationTolerance = 0.25;

/**
 * Convert a PathSpline into per-subpath curve spans, splitting only highly curved cubics while
 * preserving parameter ranges for later mapping back to the source commands.
 *
 * @param path Input PathSpline to segment.
 * @param tolerance Flatness tolerance used to subdivide cubic spans. Higher values preserve more
 * curves while lower values produce more, shorter spans.
 * @return SegmentedPath containing explicit spans for each drawing command.
 */
SegmentedPath SegmentPathForBoolean(const PathSpline& path, double tolerance);

}  // namespace donner::svg
