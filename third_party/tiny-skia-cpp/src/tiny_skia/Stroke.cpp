#include "tiny_skia/Stroke.h"

#include <algorithm>
#include <cmath>

namespace tiny_skia {

std::optional<StrokeDash> StrokeDash::create(std::vector<float> dashArray, float dashOffset) {
  if (dashArray.empty()) return std::nullopt;

  // Must have even number of entries.
  if (dashArray.size() % 2 != 0) return std::nullopt;

  // All entries must be non-negative and finite. Zero-length entries are valid
  // (e.g. a `0 N` pattern renders as caps/dots), matching upstream tiny-skia's
  // `StrokeDash::new` (which only rejects negatives).
  for (float v : dashArray) {
    if (v < 0.0f || !std::isfinite(v)) return std::nullopt;
  }

  // The total interval length must be strictly positive (an all-zero pattern is invalid).
  float intervalLen = 0.0f;
  for (float v : dashArray) {
    intervalLen += v;
  }
  if (!(intervalLen > 0.0f)) return std::nullopt;

  if (!std::isfinite(dashOffset)) return std::nullopt;

  StrokeDash result;
  result.array = std::move(dashArray);
  result.offset = dashOffset;
  return result;
}

// Donner addition: the precision floor on a stroke or dash tolerance (see Stroke.h).
float PrecisionLimitedResScale(float resScale, float maxCoordinate, float toleranceAtUnitScale) {
  // Float steps of the largest coordinate the tolerance must stay above. One is the smallest that
  // keeps subdivision bounded (half a step does not); every further step is visible error.
  constexpr float kToleranceSteps = 1.0f;
  if (!std::isfinite(maxCoordinate) || !(maxCoordinate > 0.0f) || !(resScale > 0.0f)) {
    return resScale;
  }
  float step =
      std::nextafter(maxCoordinate, std::numeric_limits<float>::infinity()) - maxCoordinate;
  if (!std::isfinite(step)) {
    // The largest finite float has no step above it; the one below is as wide.
    step = maxCoordinate - std::nextafter(maxCoordinate, 0.0f);
  }
  const float maxResScale = toleranceAtUnitScale / (kToleranceSteps * step);
  return std::isfinite(maxResScale) && maxResScale > 0.0f ? std::min(resScale, maxResScale)
                                                          : resScale;
}

float MaxAbsCoordinate(const Path& path, float reach) {
  const Rect bounds = path.bounds();
  return std::max({std::abs(bounds.left()), std::abs(bounds.right()), std::abs(bounds.top()),
                   std::abs(bounds.bottom())}) +
         std::abs(reach);
}

}  // namespace tiny_skia
