#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "tiny_skia/wide/F32x16T.h"
#include "tiny_skia/wide/U16x16T.h"

namespace tiny_skia::wide::backend::scalar {

[[nodiscard]] inline F32x16T f32x16Abs(const F32x16T& value) {
  const auto absF = [](float x) -> float {
    return std::bit_cast<float>(std::bit_cast<std::int32_t>(x) & 0x7fffffff);
  };

  const auto lo = value.lo().lanes();
  const auto hi = value.hi().lanes();

  return F32x16T(F32x8T({absF(lo[0]), absF(lo[1]), absF(lo[2]), absF(lo[3]), absF(lo[4]),
                         absF(lo[5]), absF(lo[6]), absF(lo[7])}),
                 F32x8T({absF(hi[0]), absF(hi[1]), absF(hi[2]), absF(hi[3]), absF(hi[4]),
                         absF(hi[5]), absF(hi[6]), absF(hi[7])}));
}

[[nodiscard]] inline F32x16T f32x16Sqrt(const F32x16T& value) {
  const auto lo = value.lo().lanes();
  const auto hi = value.hi().lanes();

  return F32x16T(F32x8T({std::sqrt(lo[0]), std::sqrt(lo[1]), std::sqrt(lo[2]), std::sqrt(lo[3]),
                         std::sqrt(lo[4]), std::sqrt(lo[5]), std::sqrt(lo[6]), std::sqrt(lo[7])}),
                 F32x8T({std::sqrt(hi[0]), std::sqrt(hi[1]), std::sqrt(hi[2]), std::sqrt(hi[3]),
                         std::sqrt(hi[4]), std::sqrt(hi[5]), std::sqrt(hi[6]), std::sqrt(hi[7])}));
}

inline void f32x16SaveToU16x16(const F32x16T& value, U16x16T& dst) {
  const auto lo = value.lo().lanes();
  const auto hi = value.hi().lanes();

  std::array<std::uint16_t, 16> lanes{};
  lanes[0] = static_cast<std::uint16_t>(lo[0]);
  lanes[1] = static_cast<std::uint16_t>(lo[1]);
  lanes[2] = static_cast<std::uint16_t>(lo[2]);
  lanes[3] = static_cast<std::uint16_t>(lo[3]);
  lanes[4] = static_cast<std::uint16_t>(lo[4]);
  lanes[5] = static_cast<std::uint16_t>(lo[5]);
  lanes[6] = static_cast<std::uint16_t>(lo[6]);
  lanes[7] = static_cast<std::uint16_t>(lo[7]);

  lanes[8] = static_cast<std::uint16_t>(hi[0]);
  lanes[9] = static_cast<std::uint16_t>(hi[1]);
  lanes[10] = static_cast<std::uint16_t>(hi[2]);
  lanes[11] = static_cast<std::uint16_t>(hi[3]);
  lanes[12] = static_cast<std::uint16_t>(hi[4]);
  lanes[13] = static_cast<std::uint16_t>(hi[5]);
  lanes[14] = static_cast<std::uint16_t>(hi[6]);
  lanes[15] = static_cast<std::uint16_t>(hi[7]);
  dst = U16x16T(lanes);
}

}  // namespace tiny_skia::wide::backend::scalar
