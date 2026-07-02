#include "donner/svg/renderer/PixelFormatUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace donner::svg {

using testing::ElementsAre;

TEST(TightlyPackRgbaRows, TightInputIsCopiedVerbatim) {
  // 2x2, already tightly packed (rowBytes == width * 4).
  const std::vector<std::uint8_t> pixels = {
      1, 2,  3,  4,  5,  6,  7,  8,   // row 0
      9, 10, 11, 12, 13, 14, 15, 16,  // row 1
  };

  EXPECT_THAT(TightlyPackRgbaRows(pixels, 2, 2, 8),
              ElementsAre(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16));
}

TEST(TightlyPackRgbaRows, DropsPerRowPadding) {
  // 2x2 with rowBytes == 12: each row carries 4 bytes of padding (0xEE) that
  // must not leak into the packed output.
  const std::vector<std::uint8_t> pixels = {
      1, 2,  3,  4,  5,  6,  7,  8,  0xEE, 0xEE, 0xEE, 0xEE,  // row 0 + padding
      9, 10, 11, 12, 13, 14, 15, 16, 0xEE, 0xEE, 0xEE, 0xEE,  // row 1 + padding
  };

  EXPECT_THAT(TightlyPackRgbaRows(pixels, 2, 2, 12),
              ElementsAre(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16));
}

TEST(TightlyPackRgbaRows, FinalRowMayOmitPadding) {
  // 1x2 with rowBytes == 8, but the buffer ends right after the last row's
  // pixels (a common snapshot layout: padding only *between* rows).
  const std::vector<std::uint8_t> pixels = {
      1, 2,  3,  4,  0xEE, 0xEE, 0xEE, 0xEE,  // row 0 + padding
      9, 10, 11, 12,                          // row 1, no trailing padding
  };

  EXPECT_THAT(TightlyPackRgbaRows(pixels, 1, 2, 8), ElementsAre(1, 2, 3, 4, 9, 10, 11, 12));
}

TEST(TightlyPackRgbaRows, ShortBufferLeavesMissingRowsZeroed) {
  // Buffer only holds one of the two declared rows; the missing row must be
  // zero-filled instead of read out of bounds.
  const std::vector<std::uint8_t> pixels = {1, 2, 3, 4, 0xEE, 0xEE, 0xEE, 0xEE};

  EXPECT_THAT(TightlyPackRgbaRows(pixels, 1, 2, 8), ElementsAre(1, 2, 3, 4, 0, 0, 0, 0));
}

}  // namespace donner::svg
