#include "donner/backends/tiny_skia_cpp/Stroke.h"

#include <cmath>

namespace donner::backends::tiny_skia_cpp {
namespace {

bool IsFiniteNonNegative(float value) {
  return std::isfinite(value) && value >= 0.0f;
}

float AdjustDashOffset(float offset, float intervalLength) {
  if (offset < 0.0f) {
    offset = -offset;
    if (offset > intervalLength) {
      offset = std::fmod(offset, intervalLength);
    }

    offset = intervalLength - offset;
    if (offset == intervalLength) {
      offset = 0.0f;
    }
    return offset;
  }

  if (offset >= intervalLength) {
    return std::fmod(offset, intervalLength);
  }

  return offset;
}

std::pair<float, size_t> FindFirstInterval(const std::vector<float>& dashArray, float dashOffset) {
  for (size_t index = 0; index < dashArray.size(); ++index) {
    const float gap = dashArray[index];
    if (dashOffset > gap || (dashOffset == gap && gap != 0.0f)) {
      dashOffset -= gap;
    } else {
      return std::make_pair(gap - dashOffset, index);
    }
  }

  return std::make_pair(dashArray[0], static_cast<size_t>(0));
}

}  // namespace

std::optional<StrokeDash> StrokeDash::Create(std::vector<float> dashArray, float dashOffset) {
  if (!std::isfinite(dashOffset)) {
    return std::nullopt;
  }

  if (dashArray.size() < 2 || dashArray.size() % 2 != 0) {
    return std::nullopt;
  }

  float intervalLength = 0.0f;
  for (float value : dashArray) {
    if (!IsFiniteNonNegative(value)) {
      return std::nullopt;
    }
    intervalLength += value;
  }

  if (!(intervalLength > 0.0f) || !std::isfinite(intervalLength)) {
    return std::nullopt;
  }

  dashOffset = AdjustDashOffset(dashOffset, intervalLength);
  const auto [firstLength, firstIndex] = FindFirstInterval(dashArray, dashOffset);

  return StrokeDash(std::move(dashArray), dashOffset, intervalLength, firstLength, firstIndex);
}

}  // namespace donner::backends::tiny_skia_cpp
