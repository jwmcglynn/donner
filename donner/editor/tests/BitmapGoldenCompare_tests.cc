/// @file
/// Tests for the shared pixelmatch bitmap comparison helpers.

#include "donner/editor/tests/BitmapGoldenCompare.h"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::tests {
namespace {

svg::RendererBitmap OpaqueWhiteBitmap() {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(4, 4);
  bitmap.rowBytes = 4u * 4u;
  bitmap.pixels.assign(bitmap.rowBytes * 4u, 255u);
  bitmap.alphaType = svg::AlphaType::Unpremultiplied;
  return bitmap;
}

/// A bitmap whose buffer holds one row fewer than its dimensions and row stride describe.
svg::RendererBitmap TruncatedBitmap() {
  svg::RendererBitmap bitmap = OpaqueWhiteBitmap();
  bitmap.pixels.resize(bitmap.rowBytes * 3u);
  return bitmap;
}

/// A bitmap whose row stride holds two of its four pixels, with a buffer sized to that stride.
svg::RendererBitmap ShortStrideBitmap() {
  svg::RendererBitmap bitmap = OpaqueWhiteBitmap();
  bitmap.rowBytes = 2u * 4u;
  bitmap.pixels.resize(bitmap.rowBytes * 4u);
  return bitmap;
}

void CompareTruncatedActual() {
  CompareBitmapToBitmap(TruncatedBitmap(), OpaqueWhiteBitmap(), "truncated_actual",
                        PixelmatchIdentityParams());
}

void CompareTruncatedExpected() {
  CompareBitmapToBitmap(OpaqueWhiteBitmap(), TruncatedBitmap(), "truncated_expected",
                        PixelmatchIdentityParams());
}

void CompareShortStride() {
  CompareBitmapToBitmap(ShortStrideBitmap(), ShortStrideBitmap(), "short_stride",
                        PixelmatchIdentityParams());
}

TEST(BitmapGoldenCompareTest, IdenticalBitmapsReportNoMismatchedPixels) {
  int mismatched = -2;
  CompareBitmapToBitmap(OpaqueWhiteBitmap(), OpaqueWhiteBitmap(), "identical",
                        PixelmatchIdentityParams(), &mismatched);
  EXPECT_EQ(mismatched, 0);
}

// A bitmap whose buffer or row stride does not match its dimensions cannot be compared, so the
// comparison must fail rather than report fewer mismatched pixels than the limit.
TEST(BitmapGoldenCompareTest, BitmapToBitmapFailsForATruncatedActualBitmap) {
  EXPECT_FATAL_FAILURE(CompareTruncatedActual(),
                       "[truncated_actual] actual bitmap size is inconsistent with rowBytes");
}

TEST(BitmapGoldenCompareTest, BitmapToBitmapFailsForATruncatedExpectedBitmap) {
  EXPECT_FATAL_FAILURE(CompareTruncatedExpected(),
                       "[truncated_expected] expected bitmap size is inconsistent with rowBytes");
}

TEST(BitmapGoldenCompareTest, BitmapToBitmapFailsForARowStrideShorterThanTheWidth) {
  EXPECT_FATAL_FAILURE(CompareShortStride(),
                       "[short_stride] actual rowBytes is shorter than its width");
}

}  // namespace
}  // namespace donner::editor::tests
