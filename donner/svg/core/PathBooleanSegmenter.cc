#include "donner/svg/core/PathBooleanSegmenter.h"

#include <algorithm>
#include <cassert>

#include "donner/base/Utils.h"

namespace donner::svg {
namespace {

constexpr int kMaxSegmentationDepth = 12;

double DistanceFromPointToLine(const Vector2d& point, const Vector2d& lineStart,
                               const Vector2d& lineEnd) {
  const Vector2d lineDelta = lineEnd - lineStart;
  const double lengthSquared = lineDelta.lengthSquared();
  if (NearZero(lengthSquared)) {
    return (point - lineStart).length();
  }

  const double t = Clamp((point - lineStart).dot(lineDelta) / lengthSquared, 0.0, 1.0);
  const Vector2d projection = lineStart + t * lineDelta;
  return (point - projection).length();
}

double MaxControlDistance(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                          const Vector2d& p3) {
  return std::max(DistanceFromPointToLine(p1, p0, p3), DistanceFromPointToLine(p2, p0, p3));
}

void SplitCubic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, const Vector2d& p3,
                double startT, double endT, double tolerance, size_t commandIndex, int depth,
                std::vector<PathCurveSpan>* spans) {
  if (depth >= kMaxSegmentationDepth || MaxControlDistance(p0, p1, p2, p3) <= tolerance) {
    spans->push_back(
        {PathSpline::CommandType::CurveTo, commandIndex, startT, endT, p0, p3, p1, p2});
    return;
  }

  // De Casteljau subdivision at t = 0.5
  const Vector2d p01 = (p0 + p1) * 0.5;
  const Vector2d p12 = (p1 + p2) * 0.5;
  const Vector2d p23 = (p2 + p3) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  const double midT = (startT + endT) * 0.5;
  SplitCubic(p0, p01, p012, p0123, startT, midT, tolerance, commandIndex, depth + 1, spans);
  SplitCubic(p0123, p123, p23, p3, midT, endT, tolerance, commandIndex, depth + 1, spans);
}

PathSubpathView* CurrentSubpath(SegmentedPath* segmented) {
  if (segmented->subpaths.empty()) {
    segmented->subpaths.push_back({});
  }
  return &segmented->subpaths.back();
}

}  // namespace

SegmentedPath SegmentPathForBoolean(const PathSpline& path, double tolerance) {
  UTILS_RELEASE_ASSERT(tolerance > 0.0);

  SegmentedPath segmented;
  if (path.empty()) {
    return segmented;
  }

  Vector2d currentPoint;
  Vector2d currentMoveTo;
  bool hasMoveTo = false;

  for (size_t commandIndex = 0; commandIndex < path.commands().size(); ++commandIndex) {
    const PathSpline::Command& command = path.commands()[commandIndex];

    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        currentPoint = path.points()[command.pointIndex];
        currentMoveTo = currentPoint;
        hasMoveTo = true;

        PathSubpathView* subpath = CurrentSubpath(&segmented);
        subpath->moveTo = currentPoint;
        continue;
      }

      case PathSpline::CommandType::LineTo: {
        assert(hasMoveTo && "LineTo without MoveTo");
        const Vector2d endPoint = path.points()[command.pointIndex];
        PathSubpathView* subpath = CurrentSubpath(&segmented);
        subpath->spans.push_back({PathSpline::CommandType::LineTo,
                                  commandIndex,
                                  0.0,
                                  1.0,
                                  currentPoint,
                                  endPoint,
                                  {},
                                  {}});
        currentPoint = endPoint;
        break;
      }

      case PathSpline::CommandType::CurveTo: {
        assert(hasMoveTo && "CurveTo without MoveTo");
        const Vector2d control1 = path.points()[command.pointIndex];
        const Vector2d control2 = path.points()[command.pointIndex + 1];
        const Vector2d endPoint = path.points()[command.pointIndex + 2];
        PathSubpathView* subpath = CurrentSubpath(&segmented);
        SplitCubic(currentPoint, control1, control2, endPoint, 0.0, 1.0, tolerance, commandIndex, 0,
                   &subpath->spans);
        currentPoint = endPoint;
        break;
      }

      case PathSpline::CommandType::ClosePath: {
        assert(hasMoveTo && "ClosePath without MoveTo");
        PathSubpathView* subpath = CurrentSubpath(&segmented);
        subpath->spans.push_back({PathSpline::CommandType::ClosePath,
                                  commandIndex,
                                  0.0,
                                  1.0,
                                  currentPoint,
                                  currentMoveTo,
                                  {},
                                  {}});
        subpath->closed = true;
        currentPoint = currentMoveTo;
        hasMoveTo = false;
        break;
      }
    }
  }

  return segmented;
}

}  // namespace donner::svg
