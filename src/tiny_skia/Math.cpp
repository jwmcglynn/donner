#include "tiny_skia/Math.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace tiny_skia {

int leftShift(int32_t value, int32_t shift) {
  return static_cast<int32_t>(static_cast<uint32_t>(value) << shift);
}

long long leftShift64(long long value, int32_t shift) {
  return static_cast<long long>(static_cast<uint64_t>(value) << shift);
}

float approxPowf(float x, float y) {
  if (x == 0.0f || x == 1.0f) {
    return x;
  }

  const auto xBits = std::bit_cast<uint32_t>(x);
  const float e = static_cast<float>(xBits) * (1.0f / static_cast<float>(1 << 23));
  const float m = std::bit_cast<float>((xBits & 0x007fffff) | 0x3f000000);

  const float log2X = e - 124.22551499f - 1.498030302f * m - 1.72587999f / (0.3520887068f + m);

  const float xf = log2X * y;
  const float f = xf - std::floor(xf);

  float a = xf + 121.2740575f;
  a -= f * 1.49012907f;
  a += 27.7280233f / (4.84252568f - f);
  a *= static_cast<float>(1 << 23);

  if (a < std::numeric_limits<float>::infinity()) {
    if (a > 0.0f) {
      return std::bit_cast<float>(static_cast<uint32_t>(std::round(a)));
    }
    return 0.0f;
  }

  return std::numeric_limits<float>::infinity();
}

}  // namespace tiny_skia
