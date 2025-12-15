#include "donner/backends/tiny_skia_cpp/Rasterizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

#include "donner/backends/tiny_skia_cpp/PathGeometry.h"
#include "donner/backends/tiny_skia_cpp/Transform.h"
#include "donner/base/Vector2.h"

namespace donner::backends::tiny_skia_cpp {

namespace {
constexpr double kCurveTolerance = 0.25;
constexpr double kCoverageEpsilon = 1e-6;

Vector2d ToVector(const PathPoint& point) {
  return {point.x, point.y};
}

bool IsCurveFlatEnough(const std::array<Vector2d, 4>& points) {
  const double chordLength = (points[3] - points[0]).length();
  const double netLength = (points[1] - points[0]).length() + (points[2] - points[1]).length() +
                           (points[3] - points[2]).length();
  return (netLength - chordLength) <= kCurveTolerance;
}

void FlattenCubic(const std::array<Vector2d, 4>& points, std::vector<Vector2d>& flattened,
                  int depth = 0) {
  if (depth > 10 || IsCurveFlatEnough(points)) {
    flattened.push_back(points[3]);
    return;
  }

  const Vector2d p01 = (points[0] + points[1]) * 0.5;
  const Vector2d p12 = (points[1] + points[2]) * 0.5;
  const Vector2d p23 = (points[2] + points[3]) * 0.5;
  const Vector2d p012 = (p01 + p12) * 0.5;
  const Vector2d p123 = (p12 + p23) * 0.5;
  const Vector2d p0123 = (p012 + p123) * 0.5;

  const std::array<Vector2d, 4> left = {points[0], p01, p012, p0123};
  const std::array<Vector2d, 4> right = {p0123, p123, p23, points[3]};

  FlattenCubic(left, flattened, depth + 1);
  FlattenCubic(right, flattened, depth + 1);
}

EdgeSegment BuildEdge(const Vector2d& start, const Vector2d& stop) {
  EdgeSegment edge;
  edge.x0 = start.x;
  edge.y0 = start.y;
  edge.x1 = stop.x;
  edge.y1 = stop.y;
  edge.slope = (edge.y1 == edge.y0) ? 0.0 : (edge.x1 - edge.x0) / (edge.y1 - edge.y0);
  edge.winding = (edge.y0 < edge.y1) ? 1 : -1;

  const double top = std::min(edge.y0, edge.y1);
  const double bottom = std::max(edge.y0, edge.y1);
  edge.firstY = static_cast<int32_t>(std::floor(top));
  edge.lastY = static_cast<int32_t>(std::ceil(bottom)) - 1;
  return edge;
}

size_t EmitSpanAA(double start, double stop, int width, AlphaRuns& runs, size_t offset) {
  double clampedStart = std::clamp(start, 0.0, static_cast<double>(width));
  double clampedStop = std::clamp(stop, 0.0, static_cast<double>(width));
  if (clampedStop <= clampedStart) {
    return offset;
  }

  const int startPixel = static_cast<int>(std::floor(clampedStart));
  const int stopPixel = static_cast<int>(std::floor(clampedStop - kCoverageEpsilon));
  if (startPixel >= width || stopPixel < 0) {
    return offset;
  }

  const int clampedStartPixel = std::max(0, startPixel);
  const int clampedStopPixel = std::min(width - 1, stopPixel);
  if (clampedStopPixel < clampedStartPixel) {
    return offset;
  }

  const double startCoverage =
      std::min(1.0, std::max(0.0, (static_cast<double>(startPixel) + 1.0) - clampedStart));
  const double stopCoverage = std::min(1.0, clampedStop - static_cast<double>(stopPixel));

  if (clampedStartPixel == clampedStopPixel) {
    const double coverage = clampedStop - clampedStart;
    const uint8_t alpha = static_cast<uint8_t>(std::lround(std::clamp(coverage, 0.0, 1.0) * 255.0));
    return runs.add(static_cast<uint32_t>(clampedStartPixel), alpha, 0, 0, alpha, offset);
  }

  const uint8_t startAlpha =
      static_cast<uint8_t>(std::lround(std::clamp(startCoverage, 0.0, 1.0) * 255.0));
  const uint8_t stopAlpha =
      static_cast<uint8_t>(std::lround(std::clamp(stopCoverage, 0.0, 1.0) * 255.0));
  const int middle = clampedStopPixel - clampedStartPixel - 1;
  const size_t middleCount = middle > 0 ? static_cast<size_t>(middle) : 0;

  return runs.add(static_cast<uint32_t>(clampedStartPixel), startAlpha, middleCount, stopAlpha, 255,
                  offset);
}

}  // namespace

std::vector<EdgeSegment> BuildEdges(const svg::PathSpline& spline, const Transform& transform) {
  std::vector<EdgeSegment> edges;
  PathIterator iter(spline);
  std::optional<PathSegment> segment = iter.next();
  if (!segment.has_value()) {
    return edges;
  }

  Vector2d current = transform.transformPosition(ToVector(segment->points[0]));
  Vector2d contourStart = current;

  while (segment.has_value()) {
    switch (segment->verb) {
      case PathVerb::kMove:
        contourStart = transform.transformPosition(ToVector(segment->points[0]));
        current = contourStart;
        break;
      case PathVerb::kLine: {
        const Vector2d next = transform.transformPosition(ToVector(segment->points[0]));
        if (current.y != next.y) {
          edges.push_back(BuildEdge(current, next));
        }
        current = next;
        break;
      }
      case PathVerb::kCubic: {
        std::array<Vector2d, 4> pts = {current,
                                       transform.transformPosition(ToVector(segment->points[0])),
                                       transform.transformPosition(ToVector(segment->points[1])),
                                       transform.transformPosition(ToVector(segment->points[2]))};
        std::vector<Vector2d> flattened;
        flattened.reserve(8);
        FlattenCubic(pts, flattened);
        Vector2d last = current;
        for (const Vector2d& p : flattened) {
          if (last.y != p.y) {
            edges.push_back(BuildEdge(last, p));
          }
          last = p;
        }
        current = last;
        break;
      }
      case PathVerb::kClose:
        if ((current - contourStart).lengthSquared() > 0.0 && current.y != contourStart.y) {
          edges.push_back(BuildEdge(current, contourStart));
        }
        current = contourStart;
        break;
    }

    segment = iter.next();
  }

  edges.erase(std::remove_if(edges.begin(), edges.end(),
                             [](const EdgeSegment& e) { return e.lastY < e.firstY; }),
              edges.end());

  return edges;
}

size_t EmitSpan(bool antiAlias, double start, double stop, int width, AlphaRuns& runs,
                size_t offset) {
  if (antiAlias) {
    return EmitSpanAA(start, stop, width, runs, offset);
  }

  const double clampedStart = std::clamp(start, 0.0, static_cast<double>(width));
  const double clampedStop = std::clamp(stop, 0.0, static_cast<double>(width));
  if (clampedStop <= clampedStart) {
    return offset;
  }

  const int startPixel = static_cast<int>(std::ceil(clampedStart));
  const int stopPixel = static_cast<int>(std::floor(clampedStop - kCoverageEpsilon));
  if (startPixel >= width || stopPixel < 0 || stopPixel < startPixel) {
    return offset;
  }

  return runs.add(static_cast<uint32_t>(startPixel), 255,
                  static_cast<size_t>(stopPixel - startPixel), 255, 255, offset);
}

Mask RasterizeFill(const svg::PathSpline& spline, int width, int height, FillRule fillRule,
                   bool antiAlias, const Transform& transform) {
  Mask mask = Mask::Create(width, height);
  if (!mask.isValid()) {
    return mask;
  }

  const std::vector<EdgeSegment> edges = BuildEdges(spline, transform);
  if (edges.empty()) {
    return mask;
  }

  std::vector<const EdgeSegment*> active;
  active.reserve(edges.size());

  for (int32_t y = 0; y < height; ++y) {
    AlphaRuns runs(static_cast<uint32_t>(width));
    active.clear();
    for (const EdgeSegment& edge : edges) {
      if (edge.coversScanline(y)) {
        active.push_back(&edge);
      }
    }
    if (active.empty()) {
      continue;
    }

    std::vector<std::pair<double, int8_t>> intersections;
    intersections.reserve(active.size());
    for (const EdgeSegment* edge : active) {
      const double x = edge->xAtScanline(y);
      intersections.emplace_back(x, edge->winding);
    }

    std::sort(intersections.begin(), intersections.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    int winding = 0;
    double spanStart = 0.0;
    bool inSpan = false;
    size_t offset = 0;
    for (const auto& [x, delta] : intersections) {
      if (fillRule == FillRule::kEvenOdd) {
        if (!inSpan) {
          spanStart = x;
        } else {
          offset = EmitSpan(antiAlias, spanStart, x, width, runs, offset);
        }
        inSpan = !inSpan;
        continue;
      }

      if (!inSpan) {
        spanStart = x;
        inSpan = true;
      }
      winding += delta;
      if (winding == 0) {
        offset = EmitSpan(antiAlias, spanStart, x, width, runs, offset);
        inSpan = false;
      }
    }

    size_t runIndex = 0;
    size_t alphaIndex = 0;
    size_t x = 0;
    std::span<uint8_t> row(mask.data() + static_cast<size_t>(y) * mask.strideBytes(),
                           static_cast<size_t>(width));
    while (true) {
      const size_t n = runs.runs()[runIndex];
      if (n == 0) {
        break;
      }
      const uint8_t coverage = runs.alpha()[alphaIndex];
      for (size_t i = 0; i < n && x < row.size(); ++i) {
        row[x++] = coverage;
      }
      runIndex += n;
      alphaIndex += n;
    }
  }

  return mask;
}

}  // namespace donner::backends::tiny_skia_cpp
