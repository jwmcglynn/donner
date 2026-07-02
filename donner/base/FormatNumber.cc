#include "donner/base/FormatNumber.h"

#include <cmath>
#include <cstdint>
#include <format>

namespace donner::detail {

std::string FormatNumberForSVG(double value) {
  if (value == std::trunc(value) && std::isfinite(value)) {
    return std::format("{}", static_cast<std::int64_t>(value));
  }
  return std::format("{}", value);
}

}  // namespace donner::detail
