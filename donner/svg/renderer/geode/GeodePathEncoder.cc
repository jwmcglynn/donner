#include "donner/svg/renderer/geode/GeodePathEncoder.h"

#include <algorithm>
#include <cmath>

#include "donner/base/FillRule.h"
#include "donner/base/MathUtils.h"
#include "donner/base/Path.h"

namespace donner::geode {

namespace {

/// Maximum number of bands — prevents runaway memory for huge paths.
constexpr uint16_t kMaxBands = 256;

/// Determine which bands a Y-monotonic curve intersects.
/// Curves are guaranteed monotonic in Y, so yMin/yMax are simply the first and last Y coords.
struct CurveYRange {
  float yMin;
  float yMax;
  float xMin;
  float xMax;
};

/// Compute the Y range and X extent of a quadratic Bézier (assumed Y-monotonic).
CurveYRange computeCurveRange(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y) {
  const float yMin = std::min({p0y, p1y, p2y});
  const float yMax = std::max({p0y, p1y, p2y});
  const float xMin = std::min({p0x, p1x, p2x});
  const float xMax = std::max({p0x, p1x, p2x});
  return {yMin, yMax, xMin, xMax};
}

}  // namespace

uint16_t GeodePathEncoder::computeBandCount(float pathHeight) {
  if (pathHeight <= 0.0f) {
    return 0;
  }

  // Target: ~32 pixels per band, minimum 1 band, maximum kMaxBands.
  const auto count = static_cast<uint16_t>(std::ceil(pathHeight / 32.0f));
  return std::clamp(count, static_cast<uint16_t>(1), kMaxBands);
}

EncodedPath GeodePathEncoder::encode(const Path& path, FillRule /*fillRule*/, double tolerance) {
  EncodedPath result;

  if (path.empty()) {
    return result;
  }

  // Step 1: Preprocess — cubic→quadratic, then split at Y-monotone points.
  const Path quadPath = path.cubicToQuadratic(tolerance);
  const Path monoPath = quadPath.toMonotonic();

  if (monoPath.empty()) {
    return result;
  }

  // Step 2: Compute bounds.
  const Box2d bounds = monoPath.bounds();
  if (bounds.isEmpty()) {
    return result;
  }

  result.pathBounds = bounds;

  const float pathHeight = static_cast<float>(result.pathBounds.height());
  if (NearZero(pathHeight)) {
    return result;
  }

  // Step 3: Extract all Y-monotonic quadratic curves and compute their Y ranges.
  struct CurveWithRange {
    EncodedPath::Curve curve;
    CurveYRange range;
  };
  std::vector<CurveWithRange> allCurves;

  // Track the current point as we iterate commands.
  Vector2d currentPoint;
  const auto points = monoPath.points();
  const auto commands = monoPath.commands();

  // Fill winding-number correctness requires every subpath to be a closed
  // region. Open subpaths (e.g., `<line>` rendered as MoveTo+LineTo with no
  // Close) would contribute a single edge to each scanline they cross —
  // that produces ±1 winding over the entire half-plane to one side of
  // the edge, spilling a huge fill where there should be none.
  //
  // To match the SVG/tiny-skia convention for `fill-rule` on open subpaths,
  // we emit an IMPLICIT closing line back to the subpath start whenever:
  //   (1) we encounter a new MoveTo after one or more drawn segments, or
  //   (2) the path ends with an open subpath.
  // For a path like M a L b (from `<line>`), the implicit close appends
  // L a, giving a path that crosses each scanline between y_a and y_b
  // exactly twice (once each direction) — winding 0 everywhere, no fill.
  //
  // Note: this closing segment is only relevant for fills. Stroke callers
  // funnel through `Path::strokeToFill` which already closes the ribbon
  // polygon, so the encoder sees a pre-closed path there and the implicit
  // close is a no-op.
  Vector2d subpathStart;
  bool subpathHasSegments = false;

  auto emitImplicitClose = [&]() {
    if (!subpathHasSegments) {
      return;
    }
    if (NearZero((currentPoint - subpathStart).lengthSquared())) {
      return;
    }
    const Vector2d mid = (currentPoint + subpathStart) * 0.5;

    const auto p0x = static_cast<float>(currentPoint.x);
    const auto p0y = static_cast<float>(currentPoint.y);
    const auto p1x = static_cast<float>(mid.x);
    const auto p1y = static_cast<float>(mid.y);
    const auto p2x = static_cast<float>(subpathStart.x);
    const auto p2y = static_cast<float>(subpathStart.y);

    allCurves.push_back(
        {{p0x, p0y, p1x, p1y, p2x, p2y}, computeCurveRange(p0x, p0y, p1x, p1y, p2x, p2y)});
    currentPoint = subpathStart;
  };

  for (const auto& cmd : commands) {
    switch (cmd.verb) {
      case Path::Verb::MoveTo:
        // Close the previous subpath if it was left open.
        emitImplicitClose();
        currentPoint = points[cmd.pointIndex];
        subpathStart = currentPoint;
        subpathHasSegments = false;
        break;

      case Path::Verb::LineTo: {
        // Convert line to a degenerate quadratic (control point = midpoint).
        const Vector2d& end = points[cmd.pointIndex];
        const Vector2d mid = (currentPoint + end) * 0.5;

        const auto p0x = static_cast<float>(currentPoint.x);
        const auto p0y = static_cast<float>(currentPoint.y);
        const auto p1x = static_cast<float>(mid.x);
        const auto p1y = static_cast<float>(mid.y);
        const auto p2x = static_cast<float>(end.x);
        const auto p2y = static_cast<float>(end.y);

        allCurves.push_back(
            {{p0x, p0y, p1x, p1y, p2x, p2y}, computeCurveRange(p0x, p0y, p1x, p1y, p2x, p2y)});
        currentPoint = end;
        subpathHasSegments = true;
        break;
      }

      case Path::Verb::QuadTo: {
        const Vector2d& control = points[cmd.pointIndex];
        const Vector2d& end = points[cmd.pointIndex + 1];

        const auto p0x = static_cast<float>(currentPoint.x);
        const auto p0y = static_cast<float>(currentPoint.y);
        const auto p1x = static_cast<float>(control.x);
        const auto p1y = static_cast<float>(control.y);
        const auto p2x = static_cast<float>(end.x);
        const auto p2y = static_cast<float>(end.y);

        allCurves.push_back(
            {{p0x, p0y, p1x, p1y, p2x, p2y}, computeCurveRange(p0x, p0y, p1x, p1y, p2x, p2y)});
        currentPoint = end;
        subpathHasSegments = true;
        break;
      }

      case Path::Verb::CurveTo: {
        // Should not appear after cubicToQuadratic, but handle gracefully by taking endpoints.
        const Vector2d& end = points[cmd.pointIndex + 2];
        const Vector2d mid = (currentPoint + end) * 0.5;

        const auto p0x = static_cast<float>(currentPoint.x);
        const auto p0y = static_cast<float>(currentPoint.y);
        const auto p1x = static_cast<float>(mid.x);
        const auto p1y = static_cast<float>(mid.y);
        const auto p2x = static_cast<float>(end.x);
        const auto p2y = static_cast<float>(end.y);

        allCurves.push_back(
            {{p0x, p0y, p1x, p1y, p2x, p2y}, computeCurveRange(p0x, p0y, p1x, p1y, p2x, p2y)});
        currentPoint = end;
        subpathHasSegments = true;
        break;
      }

      case Path::Verb::ClosePath: {
        // ClosePath draws a line back to the subpath start.
        const Vector2d& end = points[cmd.pointIndex];
        if (!NearZero((currentPoint - end).lengthSquared())) {
          const Vector2d mid = (currentPoint + end) * 0.5;

          const auto p0x = static_cast<float>(currentPoint.x);
          const auto p0y = static_cast<float>(currentPoint.y);
          const auto p1x = static_cast<float>(mid.x);
          const auto p1y = static_cast<float>(mid.y);
          const auto p2x = static_cast<float>(end.x);
          const auto p2y = static_cast<float>(end.y);

          allCurves.push_back(
              {{p0x, p0y, p1x, p1y, p2x, p2y}, computeCurveRange(p0x, p0y, p1x, p1y, p2x, p2y)});
        }
        currentPoint = end;
        // The subpath is explicitly closed — don't let the final implicit
        // close logic re-close it. A follow-on MoveTo begins a new subpath.
        subpathHasSegments = false;
        break;
      }
    }
  }

  // Flush the final subpath if the caller left it open.
  emitImplicitClose();

  if (allCurves.empty()) {
    return result;
  }

  // Step 4: Determine band count and band boundaries.
  const uint16_t bandCount = computeBandCount(pathHeight);
  const float bandHeight = pathHeight / static_cast<float>(bandCount);
  const float yBase = static_cast<float>(result.pathBounds.topLeft.y);

  // Step 5: Assign curves to bands. A curve that spans multiple bands is duplicated into each.
  // Per-band curve lists, then flattened.
  std::vector<std::vector<size_t>> bandCurveIndices(bandCount);

  for (size_t ci = 0; ci < allCurves.size(); ++ci) {
    const auto& range = allCurves[ci].range;

    // Determine which bands this curve overlaps.
    const int firstBand =
        std::max(0, static_cast<int>(std::floor((range.yMin - yBase) / bandHeight)));
    const int lastBand = std::min(static_cast<int>(bandCount) - 1,
                                  static_cast<int>(std::floor((range.yMax - yBase) / bandHeight)));

    for (int b = firstBand; b <= lastBand; ++b) {
      bandCurveIndices[static_cast<size_t>(b)].push_back(ci);
    }
  }

  // Step 6: Build the output — flatten curve data per-band into a contiguous array.
  result.bands.reserve(bandCount);
  // Estimate: most curves appear in 1-2 bands.
  result.curves.reserve(allCurves.size() * 2);

  for (uint16_t b = 0; b < bandCount; ++b) {
    const auto& indices = bandCurveIndices[b];
    if (indices.empty()) {
      continue;  // Skip empty bands entirely.
    }

    EncodedPath::Band band = {};
    band.curveStart = static_cast<uint32_t>(result.curves.size());
    band.curveCount = static_cast<uint32_t>(indices.size());
    band.yMin = yBase + static_cast<float>(b) * bandHeight;
    band.yMax = yBase + static_cast<float>(b + 1) * bandHeight;

    // Compute the X extent of curves in this band.
    float xMin = std::numeric_limits<float>::max();
    float xMax = std::numeric_limits<float>::lowest();

    for (size_t ci : indices) {
      const auto& curve = allCurves[ci];
      result.curves.push_back(curve.curve);
      xMin = std::min(xMin, curve.range.xMin);
      xMax = std::max(xMax, curve.range.xMax);
    }

    band.xMin = xMin;
    band.xMax = xMax;
    result.bands.push_back(band);
  }

  // Step 7: Generate bounding quad vertices for each band.
  // Each band produces a quad (2 triangles = 6 vertices).
  result.vertices.reserve(result.bands.size() * 6);

  for (size_t i = 0; i < result.bands.size(); ++i) {
    const auto& band = result.bands[i];
    const auto bandIdx = static_cast<uint32_t>(i);

    // Quad corners: (xMin, yMin), (xMax, yMin), (xMax, yMax), (xMin, yMax)
    // Two triangles: (0,1,2) and (0,2,3)

    // Outward normals for each edge of the quad (for dilation in vertex shader).
    // Bottom-left
    result.vertices.push_back({band.xMin, band.yMin, -1.0f, -1.0f, bandIdx});
    // Bottom-right
    result.vertices.push_back({band.xMax, band.yMin, 1.0f, -1.0f, bandIdx});
    // Top-right
    result.vertices.push_back({band.xMax, band.yMax, 1.0f, 1.0f, bandIdx});

    // Second triangle
    result.vertices.push_back({band.xMin, band.yMin, -1.0f, -1.0f, bandIdx});
    // Top-right
    result.vertices.push_back({band.xMax, band.yMax, 1.0f, 1.0f, bandIdx});
    // Top-left
    result.vertices.push_back({band.xMin, band.yMax, -1.0f, 1.0f, bandIdx});
  }

  return result;
}

}  // namespace donner::geode
