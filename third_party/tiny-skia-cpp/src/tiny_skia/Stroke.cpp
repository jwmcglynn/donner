#include "tiny_skia/Stroke.h"

namespace tiny_skia {

std::optional<StrokeDash> StrokeDash::create(std::vector<float> dashArray, float dashOffset) {
  if (dashArray.empty()) return std::nullopt;

  // Must have even number of entries.
  if (dashArray.size() % 2 != 0) return std::nullopt;

  // All entries must be > 0 and finite.
  for (float v : dashArray) {
    if (!(v > 0.0f) || !std::isfinite(v)) return std::nullopt;
  }

  if (!std::isfinite(dashOffset)) return std::nullopt;

  StrokeDash result;
  result.array = std::move(dashArray);
  result.offset = dashOffset;
  return result;
}

}  // namespace tiny_skia
