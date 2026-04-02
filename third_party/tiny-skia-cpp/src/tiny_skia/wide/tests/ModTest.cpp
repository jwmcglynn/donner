#include <cstdint>

#include "gtest/gtest.h"
#include "tiny_skia/wide/Wide.h"

namespace {

TEST(WideModTest, GenericBitBlendMatchesRustFormula) {
  constexpr std::uint32_t mask = 0xFF00FF00u;
  constexpr std::uint32_t y = 0x12345678u;
  constexpr std::uint32_t n = 0x87654321u;

  const std::uint32_t expected = n ^ ((n ^ y) & mask);
  EXPECT_EQ(tiny_skia::wide::genericBitBlend(mask, y, n), expected);
}

TEST(WideModTest, FasterMinReturnsSmallerOperand) {
  EXPECT_FLOAT_EQ(tiny_skia::wide::fasterMin(4.5f, -1.25f), -1.25f);
  EXPECT_FLOAT_EQ(tiny_skia::wide::fasterMin(-3.0f, -3.0f), -3.0f);
}

TEST(WideModTest, FasterMaxReturnsLargerOperand) {
  EXPECT_FLOAT_EQ(tiny_skia::wide::fasterMax(4.5f, -1.25f), 4.5f);
  EXPECT_FLOAT_EQ(tiny_skia::wide::fasterMax(-3.0f, -3.0f), -3.0f);
}

}  // namespace
