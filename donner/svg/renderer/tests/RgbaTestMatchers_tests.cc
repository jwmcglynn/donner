/// @file
/// Tests for the snapshot readers renderer tests share: a snapshot the renderer could not read back
/// fails the read, rather than answering as a transparent pixel or as no pixels at all.

#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::test {
namespace {

/// A 3x2 snapshot whose rows are padded to 16 bytes, with the three pixels of the first row
/// opaque, transparent and half transparent, and the second row transparent. The padding bytes
/// are opaque so a reader that ignores the row pitch counts them.
RendererBitmap PaddedSnapshot() {
  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(3, 2);
  bitmap.rowBytes = 16;
  bitmap.pixels = std::vector<uint8_t>{
      255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 128, 128, 255, 255, 255, 255,  //
      0,   0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0,   255, 255, 255, 255,  //
  };
  return bitmap;
}

TEST(RgbaTestMatchersTest, CountsNonTransparentPixelsRowByRowPitch) {
  EXPECT_THAT(CountNonTransparentPixels(PaddedSnapshot()), testing::Eq(2u));
  EXPECT_THAT(CountPixelsWhere(PaddedSnapshot(),
                               [](const std::array<uint8_t, 4>& pixel) { return pixel[2] != 0; }),
              testing::Eq(1u));
}

/// A renderer that could not read its frame back returns an empty snapshot. Counting its pixels
/// by its own extent visits none, and a count of zero reads as "nothing rendered", which an
/// assertion about absent content accepts.
TEST(RgbaTestMatchersTest, CountingAnEmptySnapshotFailsInsteadOfCountingNothing) {
  EXPECT_NONFATAL_FAILURE(CountNonTransparentPixels(RendererBitmap{}), "empty 0x0 snapshot");
}

TEST(RgbaTestMatchersTest, ReadingAPixelOfAnEmptySnapshotFails) {
  EXPECT_NONFATAL_FAILURE(PixelAt(RendererBitmap{}, 0, 0), "of an empty snapshot");
}

/// A snapshot whose pixel buffer ends before its extent does was not read back whole. Counting it
/// fails once, naming the shortfall, instead of once for every pixel past the end, and counts
/// nothing.
TEST(RgbaTestMatchersTest, CountingATruncatedSnapshotFailsOnce) {
  RendererBitmap truncated = PaddedSnapshot();
  truncated.pixels.resize(20);

  size_t count = 0;
  EXPECT_NONFATAL_FAILURE(count = CountNonTransparentPixels(truncated),
                          "3x2 snapshot whose 20 bytes end before the 28 its extent needs");
  EXPECT_THAT(count, testing::Eq(0u));
}

/// A row stride shorter than one row of pixels makes later rows overlap earlier ones, so adding up
/// rows by that stride asks for fewer bytes than the extent holds, and a snapshot that was not read
/// back whole would pass. Counting it fails once, naming the stride, and counts nothing.
TEST(RgbaTestMatchersTest, CountingASnapshotWithAnUndersizedRowStrideFailsOnce) {
  RendererBitmap overlapping;
  overlapping.dimensions = Vector2i(2, 2);
  overlapping.rowBytes = 4;
  overlapping.pixels = std::vector<uint8_t>(12, 255);

  size_t count = 0;
  EXPECT_NONFATAL_FAILURE(
      count = CountNonTransparentPixels(overlapping),
      "2x2 snapshot whose rows are 4 bytes apart, less than the 8 bytes a row of "
      "its pixels needs");
  EXPECT_THAT(count, testing::Eq(0u));
}

}  // namespace
}  // namespace donner::svg::test
