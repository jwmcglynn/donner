#include "tiny_skia/Stroke.h"

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

}  // namespace tiny_skia
