#include "donner/svg/renderer/geode/GeodePathEncoder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>

#include "donner/base/FillRule.h"
#include "donner/base/MathUtils.h"
#include "donner/base/Path.h"
#include "donner/base/Utils.h"

namespace donner::geode {

namespace {

/// Maximum number of bands - prevents runaway memory for huge paths.
constexpr uint16_t kMaxBands = 256;

/// Axis-aligned extent of a quadratic Bézier (control-point hull bounds).
struct CurveRange {
  float yMin;
  float yMax;
  float xMin;
  float xMax;
};

/// Compute the Y/X range of a quadratic Bézier from its control points.
CurveRange computeCurveRange(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y) {
  return {std::min({p0y, p1y, p2y}), std::max({p0y, p1y, p2y}), std::min({p0x, p1x, p2x}),
          std::max({p0x, p1x, p2x})};
}

struct CurveWithRange {
  EncodedPath::Curve curve;
  CurveRange range;
};

/// Extract every quadratic curve from a (pre-monotonic) path, lowering lines to
/// degenerate quadratics and implicitly closing open subpaths for winding
/// correctness. Axis-independent: the caller passes a path already split to be
/// monotonic along whichever axis it intends to band by.
///
/// Fill winding-number correctness requires every subpath to be a closed region.
/// Open subpaths (e.g. `<line>` as MoveTo+LineTo) would contribute a single edge
/// per scanline, spilling a half-plane fill. We emit an implicit closing line back
/// to the subpath start on a new MoveTo after drawn segments, and at path end.
/// Stroke callers funnel through `Path::strokeToFill` (already closed), so the
/// implicit close is a no-op there.
std::vector<CurveWithRange> extractCurves(const Path& monoPath) {
  std::vector<CurveWithRange> allCurves;

  const auto points = monoPath.points();
  const auto commands = monoPath.commands();

  Vector2d currentPoint;
  Vector2d subpathStart;
  bool subpathHasSegments = false;

  auto pushCurve = [&](double p0x, double p0y, double p1x, double p1y, double p2x, double p2y) {
    const auto a0x = static_cast<float>(p0x);
    const auto a0y = static_cast<float>(p0y);
    const auto a1x = static_cast<float>(p1x);
    const auto a1y = static_cast<float>(p1y);
    const auto a2x = static_cast<float>(p2x);
    const auto a2y = static_cast<float>(p2y);
    allCurves.push_back(
        {{a0x, a0y, a1x, a1y, a2x, a2y}, computeCurveRange(a0x, a0y, a1x, a1y, a2x, a2y)});
  };

  auto emitImplicitClose = [&]() {
    if (!subpathHasSegments) {
      return;
    }
    if (NearZero((currentPoint - subpathStart).lengthSquared())) {
      return;
    }
    const Vector2d mid = (currentPoint + subpathStart) * 0.5;
    pushCurve(currentPoint.x, currentPoint.y, mid.x, mid.y, subpathStart.x, subpathStart.y);
    currentPoint = subpathStart;
  };

  for (const auto& cmd : commands) {
    switch (cmd.verb) {
      case Path::Verb::MoveTo:
        emitImplicitClose();
        currentPoint = points[cmd.pointIndex];
        subpathStart = currentPoint;
        subpathHasSegments = false;
        break;

      case Path::Verb::LineTo: {
        // Line → degenerate quadratic (control point = midpoint).
        const Vector2d& end = points[cmd.pointIndex];
        const Vector2d mid = (currentPoint + end) * 0.5;
        pushCurve(currentPoint.x, currentPoint.y, mid.x, mid.y, end.x, end.y);
        currentPoint = end;
        subpathHasSegments = true;
        break;
      }

      case Path::Verb::QuadTo: {
        const Vector2d& control = points[cmd.pointIndex];
        const Vector2d& end = points[cmd.pointIndex + 1];
        pushCurve(currentPoint.x, currentPoint.y, control.x, control.y, end.x, end.y);
        currentPoint = end;
        subpathHasSegments = true;
        break;
      }

      case Path::Verb::CurveTo: {
        // INVARIANT: cubic-free input. All callers run cubicToQuadratic first, which
        // lowers every CurveTo into QuadTo. A raw cubic here is a hard-fail rather than
        // a silent flatten that produces wrong fill geometry.
        UTILS_RELEASE_ASSERT_MSG(false,
                                 "GeodePathEncoder::encode requires cubic-free input: run "
                                 "cubicToQuadratic before encoding (raw CurveTo reached the "
                                 "band encoder)");
        break;
      }

      case Path::Verb::ClosePath: {
        const Vector2d& end = points[cmd.pointIndex];
        if (!NearZero((currentPoint - end).lengthSquared())) {
          const Vector2d mid = (currentPoint + end) * 0.5;
          pushCurve(currentPoint.x, currentPoint.y, mid.x, mid.y, end.x, end.y);
        }
        currentPoint = end;
        // Explicitly closed - don't let the final implicit close re-close it.
        subpathHasSegments = false;
        break;
      }
    }
  }

  emitImplicitClose();
  return allCurves;
}

/// Which axis a band set partitions along.
enum class BandAxis { Y, X };

struct BandSpan {
  uint16_t first;
  uint16_t last;
};

struct BandMetrics {
  uint32_t maxCurves = 0;
  uint32_t p95Curves = 0;
  uint64_t totalReferences = 0;
};

std::optional<BandSpan> curveBandSpan(const CurveRange& range, const Box2d& bounds, BandAxis axis,
                                      uint16_t bandCount) {
  if (bandCount == 0) {
    return std::nullopt;
  }

  const bool byY = axis == BandAxis::Y;
  const float spanBase =
      byY ? static_cast<float>(bounds.topLeft.y) : static_cast<float>(bounds.topLeft.x);
  const float span = byY ? static_cast<float>(bounds.height()) : static_cast<float>(bounds.width());
  if (!(span > 0.0f) || !std::isfinite(span)) {
    return std::nullopt;
  }

  const float bandStride = span / static_cast<float>(bandCount);
  const float lo = byY ? range.yMin : range.xMin;
  const float hi = byY ? range.yMax : range.xMax;
  const float hiExclusive = std::nextafter(hi, -std::numeric_limits<float>::infinity());

  const int first = std::clamp(static_cast<int>(std::floor((lo - spanBase) / bandStride)), 0,
                               static_cast<int>(bandCount) - 1);
  const int last = std::clamp(static_cast<int>(std::floor((hiExclusive - spanBase) / bandStride)),
                              first, static_cast<int>(bandCount) - 1);
  return BandSpan{static_cast<uint16_t>(first), static_cast<uint16_t>(last)};
}

BandMetrics evaluateBandCount(const std::vector<CurveWithRange>& curves, const Box2d& bounds,
                              BandAxis axis, uint16_t bandCount) {
  std::vector<int32_t> countDeltas(static_cast<size_t>(bandCount) + 1u, 0);
  for (const CurveWithRange& curve : curves) {
    const std::optional<BandSpan> span = curveBandSpan(curve.range, bounds, axis, bandCount);
    if (!span) {
      continue;
    }
    ++countDeltas[span->first];
    if (span->last + 1u < countDeltas.size()) {
      --countDeltas[span->last + 1u];
    }
  }

  BandMetrics metrics;
  std::vector<uint32_t> nonemptyCounts;
  nonemptyCounts.reserve(bandCount);
  int32_t runningCount = 0;
  for (uint16_t band = 0; band < bandCount; ++band) {
    runningCount += countDeltas[band];
    const uint32_t count = static_cast<uint32_t>(runningCount);
    metrics.totalReferences += count;
    metrics.maxCurves = std::max(metrics.maxCurves, count);
    if (count != 0u) {
      nonemptyCounts.push_back(count);
    }
  }
  if (!nonemptyCounts.empty()) {
    std::sort(nonemptyCounts.begin(), nonemptyCounts.end());
    const size_t p95Index = (nonemptyCounts.size() * 95u + 99u) / 100u - 1u;
    metrics.p95Curves = nonemptyCounts[p95Index];
  }
  return metrics;
}

uint16_t chooseBandCount(const std::vector<CurveWithRange>& curves, const Box2d& bounds,
                         BandAxis axis) {
  if (curves.empty()) {
    return 0;
  }

  const uint16_t maxCandidate =
      static_cast<uint16_t>(std::min<size_t>(kMaxBands, std::max<size_t>(1u, curves.size())));
  uint16_t targetCount = 1;
  const size_t desiredCount = (curves.size() + 7u) / 8u;
  while (targetCount < desiredCount && targetCount < maxCandidate) {
    targetCount = std::min<uint16_t>(static_cast<uint16_t>(targetCount * 2u), maxCandidate);
  }
  const uint16_t startCount =
      std::min<uint16_t>(static_cast<uint16_t>(targetCount * 2u), maxCandidate);

  uint16_t bestCount = 1;
  BandMetrics bestMetrics = evaluateBandCount(curves, bounds, axis, bestCount);
  // A shorter worst-case band is not useful if it multiplies encode work and
  // resident reference bytes without bound. Two references per canonical curve
  // keeps the indirection table compact while still allowing strongly clustered
  // geometry to split into many cells.
  constexpr uint64_t kMaxReferenceExpansion = 2u;
  const uint64_t maxReferences = static_cast<uint64_t>(curves.size()) * kMaxReferenceExpansion;
  bool foundAllowedSplit = false;
  for (uint16_t candidate = startCount; candidate > 1u; candidate /= 2u) {
    const BandMetrics metrics = evaluateBandCount(curves, bounds, axis, candidate);
    if (metrics.totalReferences > maxReferences) {
      continue;
    }
    if (foundAllowedSplit && metrics.maxCurves > bestMetrics.maxCurves) {
      break;
    }
    const auto score =
        std::tuple(metrics.maxCurves, metrics.p95Curves, metrics.totalReferences, candidate);
    const auto bestScore = std::tuple(bestMetrics.maxCurves, bestMetrics.p95Curves,
                                      bestMetrics.totalReferences, bestCount);
    if (score < bestScore) {
      bestCount = candidate;
      bestMetrics = metrics;
    }
    foundAllowedSplit = true;
  }
  return bestCount;
}

/// Return true when a curve is provably parallel to the ray represented by `axis`.
/// Horizontal-ray data is split along Y and cannot receive a winding contribution
/// from a curve whose three Y coordinates are identical. The vertical case is the
/// exact transpose. Exact equality is intentional: nearly parallel curves still
/// carry geometry and must not be discarded.
bool isRayParallel(const CurveWithRange& curve, BandAxis axis) {
  if (axis == BandAxis::Y) {
    return curve.curve.p0y == curve.curve.p1y && curve.curve.p1y == curve.curve.p2y;
  }
  return curve.curve.p0x == curve.curve.p1x && curve.curve.p1x == curve.curve.p2x;
}

std::vector<CurveWithRange> omitRayParallelCurves(std::vector<CurveWithRange> curves,
                                                  BandAxis axis) {
  std::erase_if(curves, [axis](const CurveWithRange& curve) { return isRayParallel(curve, axis); });
  return curves;
}

double polygonArea(const std::vector<Vector2d>& polygon) {
  double twiceArea = 0.0;
  for (size_t i = 0; i < polygon.size(); ++i) {
    const Vector2d& a = polygon[i];
    const Vector2d& b = polygon[(i + 1u) % polygon.size()];
    twiceArea += a.x * b.y - a.y * b.x;
  }
  return std::abs(twiceArea) * 0.5;
}

/// Clip a counter-clockwise convex polygon against `normal dot point <= limit`.
/// `limit` already includes the conservative float-rounding pad used for curve supports.
std::vector<Vector2d> clipPolygon(std::vector<Vector2d> polygon, const Vector2d& normal,
                                  double limit) {
  if (polygon.empty()) {
    return polygon;
  }

  std::vector<Vector2d> clipped;
  clipped.reserve(polygon.size() + 1u);
  Vector2d previous = polygon.back();
  double previousDistance = previous.dot(normal) - limit;
  bool previousInside = previousDistance <= 0.0;
  for (const Vector2d& current : polygon) {
    const double currentDistance = current.dot(normal) - limit;
    const bool currentInside = currentDistance <= 0.0;
    if (currentInside != previousInside) {
      const double denominator = previousDistance - currentDistance;
      if (denominator != 0.0) {
        const double t = previousDistance / denominator;
        clipped.push_back(previous + (current - previous) * t);
      }
    }
    if (currentInside) {
      clipped.push_back(current);
    }
    previous = current;
    previousDistance = currentDistance;
    previousInside = currentInside;
  }
  return clipped;
}

std::vector<Vector2d> aabbPolygon(const Box2d& bounds) {
  return {{bounds.topLeft.x, bounds.topLeft.y},
          {bounds.bottomRight.x, bounds.topLeft.y},
          {bounds.bottomRight.x, bounds.bottomRight.y},
          {bounds.topLeft.x, bounds.bottomRight.y}};
}

std::vector<Vector2d> smallControlHull(const std::vector<CurveWithRange>& curves) {
  std::vector<Vector2d> points;
  points.reserve(curves.size() * 3u);
  for (const CurveWithRange& curve : curves) {
    points.emplace_back(curve.curve.p0x, curve.curve.p0y);
    points.emplace_back(curve.curve.p1x, curve.curve.p1y);
    points.emplace_back(curve.curve.p2x, curve.curve.p2y);
  }
  std::sort(points.begin(), points.end(), [](const Vector2d& lhs, const Vector2d& rhs) {
    return std::tie(lhs.x, lhs.y) < std::tie(rhs.x, rhs.y);
  });
  points.erase(std::unique(points.begin(), points.end(),
                           [](const Vector2d& lhs, const Vector2d& rhs) {
                             return lhs.x == rhs.x && lhs.y == rhs.y;
                           }),
               points.end());
  if (points.size() < 3u) {
    return {};
  }

  const auto cross = [](const Vector2d& origin, const Vector2d& a, const Vector2d& b) {
    return (a.x - origin.x) * (b.y - origin.y) - (a.y - origin.y) * (b.x - origin.x);
  };
  std::vector<Vector2d> hull;
  hull.reserve(points.size() + 1u);
  for (const Vector2d& point : points) {
    while (hull.size() >= 2u &&
           cross(hull[hull.size() - 2u], hull[hull.size() - 1u], point) <= 0.0) {
      hull.pop_back();
    }
    hull.push_back(point);
  }
  const size_t lowerSize = hull.size();
  for (auto it = points.rbegin() + 1; it != points.rend(); ++it) {
    while (hull.size() > lowerSize &&
           cross(hull[hull.size() - 2u], hull[hull.size() - 1u], *it) <= 0.0) {
      hull.pop_back();
    }
    hull.push_back(*it);
  }
  hull.pop_back();
  return hull;
}

/// Intersect the exact AABB with diagonal control-point support bounds. Quadratic Béziers stay
/// inside the convex hull of their control points, so the resulting polygon conservatively
/// encloses every segment while using at most eight vertices.
std::vector<Vector2d> buildSupportPolygon(const std::vector<CurveWithRange>& curves,
                                          const Box2d& bounds) {
  // Exact small hulls recover triangles and similarly simple geometry without paying an
  // O(N log N) hull sort on complex paths. Larger paths go directly to the fixed-size support
  // intersection below.
  if (curves.size() <= 16u) {
    std::vector<Vector2d> controlHull = smallControlHull(curves);
    if (controlHull.size() >= 3u && controlHull.size() <= EncodedPath::kMaxBoundingVertices) {
      return controlHull;
    }
  }

  double sumMin = std::numeric_limits<double>::infinity();
  double sumMax = -std::numeric_limits<double>::infinity();
  double differenceMin = std::numeric_limits<double>::infinity();
  double differenceMax = -std::numeric_limits<double>::infinity();
  auto include = [&](double x, double y) {
    sumMin = std::min(sumMin, x + y);
    sumMax = std::max(sumMax, x + y);
    differenceMin = std::min(differenceMin, x - y);
    differenceMax = std::max(differenceMax, x - y);
  };
  for (const CurveWithRange& curve : curves) {
    include(curve.curve.p0x, curve.curve.p0y);
    include(curve.curve.p1x, curve.curve.p1y);
    include(curve.curve.p2x, curve.curve.p2y);
  }
  if (!std::isfinite(sumMin) || !std::isfinite(sumMax) || !std::isfinite(differenceMin) ||
      !std::isfinite(differenceMax)) {
    return aabbPolygon(bounds);
  }

  const double supportScale = std::max({std::abs(sumMin), std::abs(sumMax), std::abs(differenceMin),
                                        std::abs(differenceMax), bounds.width(), bounds.height(),
                                        static_cast<double>(std::numeric_limits<float>::min())});
  const double pad = supportScale * std::numeric_limits<float>::epsilon() * 8.0;

  std::vector<Vector2d> polygon = aabbPolygon(bounds);
  polygon = clipPolygon(std::move(polygon), Vector2d(1.0, 1.0), sumMax + pad);
  polygon = clipPolygon(std::move(polygon), Vector2d(-1.0, -1.0), -sumMin + pad);
  polygon = clipPolygon(std::move(polygon), Vector2d(1.0, -1.0), differenceMax + pad);
  polygon = clipPolygon(std::move(polygon), Vector2d(-1.0, 1.0), -differenceMin + pad);
  return polygon;
}

bool hasBoundedIdentityMiters(const std::vector<Vector2d>& polygon) {
  constexpr double kMaxMiterPixels = 2.0;
  for (size_t i = 0; i < polygon.size(); ++i) {
    const Vector2d previous = polygon[(i + polygon.size() - 1u) % polygon.size()];
    const Vector2d position = polygon[i];
    const Vector2d next = polygon[(i + 1u) % polygon.size()];
    const Vector2d incoming = position - previous;
    const Vector2d outgoing = next - position;
    const double incomingLength = incoming.length();
    const double outgoingLength = outgoing.length();
    if (!(incomingLength > 0.0) || !(outgoingLength > 0.0) || !std::isfinite(incomingLength) ||
        !std::isfinite(outgoingLength)) {
      return false;
    }

    const Vector2d incomingNormal(incoming.y / incomingLength, -incoming.x / incomingLength);
    const Vector2d outgoingNormal(outgoing.y / outgoingLength, -outgoing.x / outgoingLength);
    const double denominator = 1.0 + incomingNormal.dot(outgoingNormal);
    if (!(denominator > 0.0) || !std::isfinite(denominator)) {
      return false;
    }
    const Vector2d pixelDelta = 0.5 * (incomingNormal + outgoingNormal) / denominator;
    if (!std::isfinite(pixelDelta.x) || !std::isfinite(pixelDelta.y) ||
        pixelDelta.length() > kMaxMiterPixels) {
      return false;
    }
  }
  return true;
}

bool isValidFloatBoundingPolygon(
    const std::array<Vector2f, EncodedPath::kMaxBoundingVertices>& vertices, size_t count) {
  if (count < 3u || count > vertices.size()) {
    return false;
  }
  const Vector2f origin = vertices[0];
  double twiceArea = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const Vector2f& previous = vertices[(i + count - 1u) % count];
    const Vector2f& position = vertices[i];
    const Vector2f& next = vertices[(i + 1u) % count];
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        (position.x == next.x && position.y == next.y)) {
      return false;
    }
    const double incomingX = static_cast<double>(position.x) - previous.x;
    const double incomingY = static_cast<double>(position.y) - previous.y;
    const double outgoingX = static_cast<double>(next.x) - position.x;
    const double outgoingY = static_cast<double>(next.y) - position.y;
    if (!(incomingX * outgoingY - incomingY * outgoingX > 0.0)) {
      return false;
    }
    twiceArea +=
        (static_cast<double>(position.x) - origin.x) * (static_cast<double>(next.y) - origin.y) -
        (static_cast<double>(position.y) - origin.y) * (static_cast<double>(next.x) - origin.x);
  }
  return twiceArea > 0.0 && std::isfinite(twiceArea);
}

void storeBoundingPolygon(EncodedPath& result, const std::vector<CurveWithRange>& curves,
                          const Box2d& bounds) {
  const double aabbArea = bounds.width() * bounds.height();
  std::vector<Vector2d> polygon = buildSupportPolygon(curves, bounds);
  double area = polygon.size() >= 3u ? polygonArea(polygon) : aabbArea;

  // Extra support edges are worthwhile only when their fragment-area saving grows with the
  // extra triangle count. Triangles always beat their AABB when valid; larger polygons must
  // save 20%, plus 5% for every triangle beyond the AABB's two.
  const size_t triangleCount = polygon.size() >= 3u ? polygon.size() - 2u : 0u;
  const size_t extraTriangles = triangleCount > 2u ? triangleCount - 2u : 0u;
  const double requiredSavingFraction = 0.20 + 0.05 * static_cast<double>(extraTriangles);
  const bool validPolygon = polygon.size() >= 3u &&
                            polygon.size() <= EncodedPath::kMaxBoundingVertices &&
                            std::isfinite(area) && area > 0.0 && hasBoundedIdentityMiters(polygon);
  bool useSupportPolygon = validPolygon && (aabbArea - area) >= aabbArea * requiredSavingFraction;
  if (!useSupportPolygon) {
    polygon = aabbPolygon(bounds);
    area = aabbArea;
  }

  auto convertPolygon = [&](bool supportPolygon) {
    std::array<Vector2f, EncodedPath::kMaxBoundingVertices> converted = {};
    Vector2d center(0.0, 0.0);
    for (const Vector2d& point : polygon) {
      center += point;
    }
    center /= static_cast<double>(polygon.size());
    const double conservativeScale =
        supportPolygon ? 1.0 + 16.0 * std::numeric_limits<float>::epsilon() : 1.0;
    for (size_t i = 0; i < polygon.size(); ++i) {
      const Vector2d expanded = center + (polygon[i] - center) * conservativeScale;
      float x = static_cast<float>(expanded.x);
      float y = static_cast<float>(expanded.y);
      if (supportPolygon) {
        x = std::nextafter(x, x >= center.x ? std::numeric_limits<float>::infinity()
                                            : -std::numeric_limits<float>::infinity());
        y = std::nextafter(y, y >= center.y ? std::numeric_limits<float>::infinity()
                                            : -std::numeric_limits<float>::infinity());
      } else {
        const bool left = i == 0u || i == 3u;
        const bool top = i < 2u;
        x = std::nextafter(x, left ? -std::numeric_limits<float>::infinity()
                                   : std::numeric_limits<float>::infinity());
        y = std::nextafter(y, top ? -std::numeric_limits<float>::infinity()
                                  : std::numeric_limits<float>::infinity());
      }
      converted[i] = {x, y};
    }
    return converted;
  };

  auto converted = convertPolygon(useSupportPolygon);
  if (useSupportPolygon && !isValidFloatBoundingPolygon(converted, polygon.size())) {
    polygon = aabbPolygon(bounds);
    area = aabbArea;
    useSupportPolygon = false;
    converted = convertPolygon(false);
  }
  if (!isValidFloatBoundingPolygon(converted, polygon.size())) {
    result.boundingVertexCount = 0u;
    result.stats.boundingGeometryArea = 0.0;
    result.stats.boundingGeometryVertexCount = 0u;
    return;
  }

  result.boundingVertexCount = static_cast<uint32_t>(polygon.size());
  for (size_t i = 0; i < polygon.size(); ++i) {
    result.boundingVertices[i] = {converted[i].x, converted[i].y};
  }
  result.stats.boundingGeometryArea = area;
  result.stats.boundingGeometryVertexCount = result.boundingVertexCount;
}

/// Bin curves into bands along `axis` and flatten into the band/curve output vectors.
/// For `BandAxis::Y` (horizontal bands, horizontal ray) the band's `yMin`/`yMax` are
/// its strip boundaries and `xMin`/`xMax` are the curves' X-extent. For `BandAxis::X`
/// (vertical bands, vertical ray) the roles transpose: `xMin`/`xMax` are the strip
/// boundaries and `yMin`/`yMax` are the curves' Y-extent.
void bandCurves(const std::vector<CurveWithRange>& allCurves, const Box2d& bounds, BandAxis axis,
                std::vector<EncodedPath::Band>& outBands,
                std::vector<EncodedPath::Curve>& outCurves, std::vector<uint32_t>& outCurveIndices,
                uint16_t bandCount, std::vector<uint32_t>& outGrid,
                EncodedPath::AxisStats& outStats) {
  // Dense cell → band-slot map (kNoBand for empty cells). Sized to the full grid even when
  // some cells are empty, so the fragment shader can index it directly by position.
  outGrid.assign(bandCount, EncodedPath::kNoBand);
  if (bandCount == 0 || allCurves.empty()) {
    return;
  }

  UTILS_RELEASE_ASSERT_MSG(
      outCurves.size() <= std::numeric_limits<uint32_t>::max() &&
          allCurves.size() <= std::numeric_limits<uint32_t>::max() - outCurves.size(),
      "Geode path has too many canonical curves for 32-bit references");
  const uint32_t canonicalBase = static_cast<uint32_t>(outCurves.size());
  outCurves.reserve(outCurves.size() + allCurves.size());
  for (const CurveWithRange& curve : allCurves) {
    outCurves.push_back(curve.curve);
  }

  const bool byY = (axis == BandAxis::Y);

  // Sort the canonical curve indexes once. Appending them to every overlapping
  // band in this order preserves descending cross-axis maxima without an O(B)
  // family of per-band sorts.
  std::vector<size_t> sortedCurveIndices(allCurves.size());
  std::iota(sortedCurveIndices.begin(), sortedCurveIndices.end(), 0u);
  std::sort(sortedCurveIndices.begin(), sortedCurveIndices.end(), [&](size_t lhs, size_t rhs) {
    const CurveRange& lhsRange = allCurves[lhs].range;
    const CurveRange& rhsRange = allCurves[rhs].range;
    const float lhsMax = byY ? lhsRange.xMax : lhsRange.yMax;
    const float rhsMax = byY ? rhsRange.xMax : rhsRange.yMax;
    return lhsMax != rhsMax ? lhsMax > rhsMax : lhs < rhs;
  });

  // Build the compact row offsets directly. This avoids one heap allocation per band while
  // preserving the globally sorted reference order inside every row.
  std::vector<uint32_t> bandCounts(bandCount, 0u);
  for (const CurveWithRange& curve : allCurves) {
    const std::optional<BandSpan> curveSpan = curveBandSpan(curve.range, bounds, axis, bandCount);
    if (!curveSpan) {
      continue;
    }
    for (uint16_t band = curveSpan->first; band <= curveSpan->last; ++band) {
      ++bandCounts[band];
    }
  }

  outBands.reserve(outBands.size() + bandCount);
  const size_t referenceBase = outCurveIndices.size();
  uint64_t totalReferences = 0u;
  std::vector<uint32_t> nonemptyCounts;
  nonemptyCounts.reserve(bandCount);

  for (uint16_t b = 0; b < bandCount; ++b) {
    const uint32_t count = bandCounts[b];
    if (count == 0u) {
      continue;  // Skip empty bands entirely.
    }

    // Record the dense-grid cell → packed-band-slot mapping for O(1) shader lookup.
    outGrid[b] = static_cast<uint32_t>(outBands.size());
    UTILS_RELEASE_ASSERT_MSG(
        referenceBase + totalReferences <= std::numeric_limits<uint32_t>::max() &&
            count <= std::numeric_limits<uint32_t>::max() - referenceBase - totalReferences,
        "Geode path band references overflow 32-bit storage");
    outBands.push_back(
        {static_cast<uint32_t>(referenceBase + totalReferences), static_cast<uint32_t>(count)});
    totalReferences += count;
    nonemptyCounts.push_back(count);
  }

  UTILS_RELEASE_ASSERT_MSG(totalReferences <= std::numeric_limits<size_t>::max() - referenceBase,
                           "Geode path band references overflow addressable storage");
  outCurveIndices.resize(referenceBase + static_cast<size_t>(totalReferences));
  std::vector<uint32_t> writeOffsets(bandCount, 0u);
  for (uint16_t b = 0; b < bandCount; ++b) {
    const uint32_t slot = outGrid[b];
    if (slot != EncodedPath::kNoBand) {
      writeOffsets[b] = outBands[slot].curveStart;
    }
  }
  for (size_t ci : sortedCurveIndices) {
    const std::optional<BandSpan> curveSpan =
        curveBandSpan(allCurves[ci].range, bounds, axis, bandCount);
    if (!curveSpan) {
      continue;
    }
    for (uint16_t band = curveSpan->first; band <= curveSpan->last; ++band) {
      outCurveIndices[writeOffsets[band]++] = canonicalBase + static_cast<uint32_t>(ci);
    }
  }

  std::sort(nonemptyCounts.begin(), nonemptyCounts.end());
  outStats.canonicalCurveCount = static_cast<uint32_t>(allCurves.size());
  outStats.curveReferenceCount = static_cast<uint32_t>(totalReferences);
  outStats.gridBandCount = bandCount;
  outStats.nonemptyBandCount = static_cast<uint32_t>(outBands.size());
  if (!nonemptyCounts.empty()) {
    outStats.maxCurvesPerBand = nonemptyCounts.back();
    const size_t p95Index = (nonemptyCounts.size() * 95u + 99u) / 100u - 1u;
    outStats.p95CurvesPerBand = nonemptyCounts[p95Index];
    outStats.meanCurvesPerBand =
        static_cast<double>(totalReferences) / static_cast<double>(nonemptyCounts.size());
  }
}

}  // namespace

EncodedPath GeodePathEncoder::encode(const Path& path, FillRule /*fillRule*/, double tolerance) {
  EncodedPath result;

  if (path.empty()) {
    return result;
  }

  // Cubic→quadratic once; the two monotonic splits share it.
  const Path quadPath = path.cubicToQuadratic(tolerance);
  if (quadPath.empty()) {
    return result;
  }

  // Bounds from the Y-monotonic form (same point set as X-monotonic; bounds are identical).
  const Path monoPathY = quadPath.toMonotonic(Path::MonotonicAxis::Y);
  if (monoPathY.empty()) {
    return result;
  }
  const Box2d bounds = monoPathY.bounds();
  if (bounds.isEmpty()) {
    return result;
  }
  result.pathBounds = bounds;
  result.stats.aabbArea = bounds.width() * bounds.height();

  const float pathHeight = static_cast<float>(bounds.height());
  const float pathWidth = static_cast<float>(bounds.width());
  if (NearZero(pathHeight)) {
    return result;
  }

  // Horizontal bands (Y-monotonic curves) for the horizontal ray.
  const std::vector<CurveWithRange> hAll = extractCurves(monoPathY);
  storeBoundingPolygon(result, hAll, bounds);
  if (result.boundingVertexCount < 3u) {
    return result;
  }
  const std::vector<CurveWithRange> hCurves = omitRayParallelCurves(hAll, BandAxis::Y);
  result.stats.horizontal.omittedParallelCurves =
      static_cast<uint32_t>(hAll.size() - hCurves.size());
  if (hCurves.empty()) {
    return result;
  }
  const uint16_t hBandCount = chooseBandCount(hCurves, bounds, BandAxis::Y);
  bandCurves(hCurves, bounds, BandAxis::Y, result.bands, result.curves, result.curveIndices,
             hBandCount, result.hBandGrid, result.stats.horizontal);
  if (result.bands.empty()) {
    return result;
  }
  result.yBase = static_cast<float>(bounds.topLeft.y);
  result.hStride = pathHeight / static_cast<float>(hBandCount);
  result.hBandCount = hBandCount;

  // Vertical bands (X-monotonic curves) for the Slug vertical ray (dual-ray coverage).
  // Degenerate-width paths (e.g. a vertical hairline) skip vertical banding; the
  // horizontal ray alone covers them.
  if (!NearZero(pathWidth)) {
    const Path monoPathX = quadPath.toMonotonic(Path::MonotonicAxis::X);
    const std::vector<CurveWithRange> vExtracted = extractCurves(monoPathX);
    const std::vector<CurveWithRange> vAll = omitRayParallelCurves(vExtracted, BandAxis::X);
    result.stats.vertical.omittedParallelCurves =
        static_cast<uint32_t>(vExtracted.size() - vAll.size());
    if (!vAll.empty()) {
      const uint16_t vBandCount = chooseBandCount(vAll, bounds, BandAxis::X);
      bandCurves(vAll, bounds, BandAxis::X, result.vBands, result.vCurves, result.vCurveIndices,
                 vBandCount, result.vBandGrid, result.stats.vertical);
      result.xBase = static_cast<float>(bounds.topLeft.x);
      result.vStride = pathWidth / static_cast<float>(vBandCount);
      result.vBandCount = vBandCount;
    }
  }

  return result;
}

}  // namespace donner::geode
