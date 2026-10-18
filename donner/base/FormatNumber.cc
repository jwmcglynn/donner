#include "donner/base/FormatNumber.h"

#include <cmath>
#include <cstdint>
#include <format>

namespace donner::detail {

std::string FormatNumberForSVG(double value) {
  // The positive boundary is exclusive: converting INT64_MAX to double rounds up to 2^63.
  constexpr double kMinimumInteger = -0x1p63;
  constexpr double kIntegerUpperBound = 0x1p63;
  if (value == std::trunc(value) && std::isfinite(value) && value >= kMinimumInteger &&
      value < kIntegerUpperBound) {
    return std::format("{}", static_cast<std::int64_t>(value));
  }
  return std::format("{}", value);
}

}  // namespace donner::detail
