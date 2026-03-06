#pragma once

#include <cstdint>

#include "tiny_skia/Math.h"

namespace tiny_skia {

using FDot6 = std::int32_t;
using FDot8 = std::int32_t;
using FDot16 = std::int32_t;

namespace fdot6 {

constexpr FDot6 one = 64;

FDot6 fromI32(std::int32_t n);
FDot6 fromF32(float n);
FDot6 floor(FDot6 n);
FDot6 ceil(FDot6 n);
FDot6 round(FDot6 n);
FDot16 toFdot16(FDot6 n);
FDot16 div(FDot6 a, FDot6 b);
bool canConvertToFdot16(FDot6 n);
std::uint8_t smallScale(std::uint8_t value, FDot6 dot6);

}  // namespace fdot6

namespace fdot8 {

FDot8 fromFdot16(FDot16 x);

}  // namespace fdot8

namespace fdot16 {

constexpr FDot16 half = (1 << 16) / 2;
constexpr FDot16 one = 1 << 16;

FDot16 fromF32(float x);
std::int32_t floorToI32(FDot16 x);
std::int32_t ceilToI32(FDot16 x);
std::int32_t roundToI32(FDot16 x);
FDot16 mul(FDot16 a, FDot16 b);
FDot16 divide(FDot6 numer, FDot6 denom);
FDot16 fastDiv(FDot6 a, FDot6 b);

}  // namespace fdot16

}  // namespace tiny_skia
