#include "donner/svg/renderer/geode/GeodePathEncoder.h"

#include <algorithm>
#include <cmath>

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

/// Bin curves into bands along `axis` and flatten into the band/curve output vectors.
/// For `BandAxis::Y` (horizontal bands, horizontal ray) the band's `yMin`/`yMax` are
/// its strip boundaries and `xMin`/`xMax` are the curves' X-extent. For `BandAxis::X`
/// (vertical bands, vertical ray) the roles transpose: `xMin`/`xMax` are the strip
/// boundaries and `yMin`/`yMax` are the curves' Y-extent.
void bandCurves(const std::vector<CurveWithRange>& allCurves, const Box2d& bounds, BandAxis axis,
                std::vector<EncodedPath::Band>& outBands,
                std::vector<EncodedPath::Curve>& outCurves, uint16_t bandCount,
                std::vector<uint32_t>& outGrid) {
  // Dense cell → band-slot map (kNoBand for empty cells). Sized to the full grid even when
  // some cells are empty, so the fragment shader can index it directly by position.
  outGrid.assign(bandCount, EncodedPath::kNoBand);
  if (bandCount == 0 || allCurves.empty()) {
    return;
  }

  const bool byY = (axis == BandAxis::Y);
  const float spanBase =
      byY ? static_cast<float>(bounds.topLeft.y) : static_cast<float>(bounds.topLeft.x);
  const float span = byY ? static_cast<float>(bounds.height()) : static_cast<float>(bounds.width());
  const float bandStride = span / static_cast<float>(bandCount);

  // Per-band curve index lists.
  std::vector<std::vector<size_t>> bandCurveIndices(bandCount);
  for (size_t ci = 0; ci < allCurves.size(); ++ci) {
    const auto& range = allCurves[ci].range;
    const float lo = byY ? range.yMin : range.xMin;
    const float hi = byY ? range.yMax : range.xMax;
    const int firstBand = std::max(0, static_cast<int>(std::floor((lo - spanBase) / bandStride)));
    const int lastBand = std::min(static_cast<int>(bandCount) - 1,
                                  static_cast<int>(std::floor((hi - spanBase) / bandStride)));
    for (int b = firstBand; b <= lastBand; ++b) {
      bandCurveIndices[static_cast<size_t>(b)].push_back(ci);
    }
  }

  outBands.reserve(outBands.size() + bandCount);
  outCurves.reserve(outCurves.size() + allCurves.size() * 2);

  for (uint16_t b = 0; b < bandCount; ++b) {
    auto& indices = bandCurveIndices[b];
    if (indices.empty()) {
      continue;  // Skip empty bands entirely.
    }

    // The fragment shader scans a positive ray. Descending cross-axis maxima let it
    // stop as soon as a curve's conservative control-hull maximum lies more than
    // half a pixel behind the sample, because every remaining curve is no larger.
    std::stable_sort(indices.begin(), indices.end(), [&](size_t lhs, size_t rhs) {
      const CurveRange& lhsRange = allCurves[lhs].range;
      const CurveRange& rhsRange = allCurves[rhs].range;
      const float lhsMax = byY ? lhsRange.xMax : lhsRange.yMax;
      const float rhsMax = byY ? rhsRange.xMax : rhsRange.yMax;
      return lhsMax > rhsMax;
    });

    // Record the dense-grid cell → packed-band-slot mapping for O(1) shader lookup.
    outGrid[b] = static_cast<uint32_t>(outBands.size());

    EncodedPath::Band band = {};
    band.curveStart = static_cast<uint32_t>(outCurves.size());
    band.curveCount = static_cast<uint32_t>(indices.size());

    const float stripLo = spanBase + static_cast<float>(b) * bandStride;
    const float stripHi = spanBase + static_cast<float>(b + 1) * bandStride;

    // Cross-axis extent of curves in this band.
    float crossMin = std::numeric_limits<float>::max();
    float crossMax = std::numeric_limits<float>::lowest();
    for (size_t ci : indices) {
      const auto& curve = allCurves[ci];
      outCurves.push_back(curve.curve);
      crossMin = std::min(crossMin, byY ? curve.range.xMin : curve.range.yMin);
      crossMax = std::max(crossMax, byY ? curve.range.xMax : curve.range.yMax);
    }

    if (byY) {
      band.yMin = stripLo;
      band.yMax = stripHi;
      band.xMin = crossMin;
      band.xMax = crossMax;
    } else {
      band.xMin = stripLo;
      band.xMax = stripHi;
      band.yMin = crossMin;
      band.yMax = crossMax;
    }
    outBands.push_back(band);
  }
}

}  // namespace

uint16_t GeodePathEncoder::computeBandCount(float pathExtent) {
  if (pathExtent <= 0.0f) {
    return 0;
  }
  // Target: ~32 pixels per band, minimum 1 band, maximum kMaxBands.
  const auto count = static_cast<uint16_t>(std::ceil(pathExtent / 32.0f));
  return std::clamp(count, static_cast<uint16_t>(1), kMaxBands);
}

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

  const float pathHeight = static_cast<float>(bounds.height());
  const float pathWidth = static_cast<float>(bounds.width());
  if (NearZero(pathHeight)) {
    return result;
  }

  // Horizontal bands (Y-monotonic curves) for the horizontal ray.
  const std::vector<CurveWithRange> hCurves =
      omitRayParallelCurves(extractCurves(monoPathY), BandAxis::Y);
  if (hCurves.empty()) {
    return result;
  }
  const uint16_t hBandCount = computeBandCount(pathHeight);
  bandCurves(hCurves, bounds, BandAxis::Y, result.bands, result.curves, hBandCount,
             result.hBandGrid);
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
    const std::vector<CurveWithRange> vAll =
        omitRayParallelCurves(extractCurves(monoPathX), BandAxis::X);
    const uint16_t vBandCount = computeBandCount(pathWidth);
    bandCurves(vAll, bounds, BandAxis::X, result.vBands, result.vCurves, vBandCount,
               result.vBandGrid);
    result.xBase = static_cast<float>(bounds.topLeft.x);
    result.vStride = pathWidth / static_cast<float>(vBandCount);
    result.vBandCount = vBandCount;
  }

  // Single bounding quad over the whole path for the analytic dual-ray fill shader.
  // One quad → each pixel shaded once → folded coverage composes (no seam double-count).
  {
    const auto qxMin = static_cast<float>(bounds.topLeft.x);
    const auto qyMin = static_cast<float>(bounds.topLeft.y);
    const auto qxMax = static_cast<float>(bounds.bottomRight.x);
    const auto qyMax = static_cast<float>(bounds.bottomRight.y);
    result.quadVertices.reserve(6);
    result.quadVertices.push_back({qxMin, qyMin, -1.0f, -1.0f, 0u});
    result.quadVertices.push_back({qxMax, qyMin, 1.0f, -1.0f, 0u});
    result.quadVertices.push_back({qxMax, qyMax, 1.0f, 1.0f, 0u});
    result.quadVertices.push_back({qxMin, qyMin, -1.0f, -1.0f, 0u});
    result.quadVertices.push_back({qxMax, qyMax, 1.0f, 1.0f, 0u});
    result.quadVertices.push_back({qxMin, qyMax, -1.0f, 1.0f, 0u});
  }

  return result;
}

}  // namespace donner::geode
