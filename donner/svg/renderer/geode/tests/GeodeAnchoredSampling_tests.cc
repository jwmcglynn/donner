#include "donner/svg/renderer/geode/GeodeAnchoredSampling.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <optional>

namespace donner::geode {
namespace {

/// The skewed local-raster resample of a filter under matrix(1 0 1 1 0 0): local raster pixels
/// from capture texels, with the local raster's y axis scaled by sqrt(2), for content placed
/// \p contentX texels into the capture.
Transform2d SkewedLocalFromCapture(double contentX) {
  Transform2d skew;
  skew.data[2] = 1.0;
  return Transform2d::Translate(-contentX, 0.0) * skew.inverse() *
         Transform2d::Translate(8.0, 0.0) * Transform2d::Scale(1.0, std::sqrt(2.0));
}

// Moving the source by whole texels changes only the whole part, so every bilinear weight stays
// the same; a single-precision sum with the absolute texel coordinate does not guarantee that.
TEST(GeodeAnchoredSampling, WholeTexelSourceMoveChangesOnlyTheWholePart) {
  const std::optional<AnchoredTexelRows> atEdge =
      AnchoredTexelRowsFor(SkewedLocalFromCapture(0.0), Vector2i::Zero());
  const std::optional<AnchoredTexelRows> withRoom =
      AnchoredTexelRowsFor(SkewedLocalFromCapture(32.0), Vector2i::Zero());
  ASSERT_THAT(atEdge, testing::Optional(testing::_));
  ASSERT_THAT(withRoom, testing::Optional(testing::_));
  EXPECT_EQ(atEdge->linear, withRoom->linear);
  EXPECT_EQ(atEdge->fraction, withRoom->fraction);
  EXPECT_EQ(withRoom->whole - atEdge->whole, Vector2i(32, 0));
}

// Moving the target and the anchor together by whole pixels changes nothing.
TEST(GeodeAnchoredSampling, WholePixelTargetMoveWithTheAnchorChangesNothing) {
  const Transform2d targetFromTexel = SkewedLocalFromCapture(0.0);
  const std::optional<AnchoredTexelRows> base =
      AnchoredTexelRowsFor(targetFromTexel, Vector2i(3, 5));
  const std::optional<AnchoredTexelRows> moved =
      AnchoredTexelRowsFor(targetFromTexel * Transform2d::Translate(32.0, -7.0), Vector2i(35, -2));
  ASSERT_THAT(base, testing::Optional(testing::_));
  ASSERT_THAT(moved, testing::Optional(testing::_));
  EXPECT_EQ(base->linear, moved->linear);
  EXPECT_EQ(base->fraction, moved->fraction);
  EXPECT_EQ(base->whole, moved->whole);
}

// The split reproduces the anchor's texel coordinate, with a fraction in [0, 1).
TEST(GeodeAnchoredSampling, SplitsTheAnchorTexelCoordinate) {
  const Transform2d targetFromTexel =
      Transform2d::Scale(2.0, 4.0) * Transform2d::Translate(-3.25, 10.5);
  const std::optional<AnchoredTexelRows> rows =
      AnchoredTexelRowsFor(targetFromTexel, Vector2i(1, 2));
  ASSERT_THAT(rows, testing::Optional(testing::_));
  // texel = ((x + 3.25) / 2, (y - 10.5) / 4), so target (1, 2) is texel (2.125, -2.125).
  EXPECT_EQ(rows->whole, Vector2i(2, -3));
  EXPECT_FLOAT_EQ(rows->fraction[0], 0.125f);
  EXPECT_FLOAT_EQ(rows->fraction[1], 0.875f);
  EXPECT_FLOAT_EQ(rows->linear[0], 0.5f);
  EXPECT_FLOAT_EQ(rows->linear[1], 0.0f);
  EXPECT_FLOAT_EQ(rows->linear[2], 0.0f);
  EXPECT_FLOAT_EQ(rows->linear[3], 0.25f);
  EXPECT_EQ(rows->anchor, Vector2i(1, 2));
}

// An anchor on a texel boundary up to double rounding lands on that texel, not beside it.
TEST(GeodeAnchoredSampling, AnchorOnAWholeTexelHasNoFraction) {
  const std::optional<AnchoredTexelRows> below =
      AnchoredTexelRowsFor(Transform2d::Translate(-(5.0 - 1e-12), 0.0), Vector2i::Zero());
  const std::optional<AnchoredTexelRows> above =
      AnchoredTexelRowsFor(Transform2d::Translate(-(5.0 + 1e-12), 0.0), Vector2i::Zero());
  ASSERT_THAT(below, testing::Optional(testing::_));
  ASSERT_THAT(above, testing::Optional(testing::_));
  EXPECT_EQ(below->whole, Vector2i(5, 0));
  EXPECT_EQ(above->whole, Vector2i(5, 0));
  EXPECT_EQ(below->fraction[0], 0.0f);
  EXPECT_EQ(above->fraction[0], 0.0f);
}

// The anchor pixel of a position moves by whole pixels with the position, including a position on
// a pixel boundary that double rounding puts a hair to either side.
TEST(GeodeAnchoredSampling, AnchorPixelMovesWithItsPosition) {
  EXPECT_THAT(AnchorPixelFor(Vector2d(3.7, -2.2)), testing::Optional(Vector2i(3, -3)));
  EXPECT_THAT(AnchorPixelFor(Vector2d(5.0 - 1e-12, 2.0 + 1e-12)),
              testing::Optional(Vector2i(5, 2)));
  EXPECT_THAT(AnchorPixelFor(Vector2d(37.0 - 1e-12, 2.0)), testing::Optional(Vector2i(37, 2)));
}

// Anchors past 2^24, where a float no longer holds every integer, still split exactly, and a
// position beyond the int range or not finite has no anchor.
TEST(GeodeAnchoredSampling, LargeAndOutOfRangeAnchors) {
  constexpr int kLarge = (1 << 24) + 3;
  EXPECT_THAT(AnchorPixelFor(Vector2d(kLarge + 0.5, -kLarge - 0.5)),
              testing::Optional(Vector2i(kLarge, -kLarge - 1)));
  EXPECT_THAT(AnchorPixelFor(Vector2d(3e9, 0.0)), testing::Eq(std::nullopt));
  EXPECT_THAT(AnchorPixelFor(Vector2d(0.0, -3e9)), testing::Eq(std::nullopt));
  EXPECT_THAT(AnchorPixelFor(Vector2d(std::nan(""), 0.0)), testing::Eq(std::nullopt));

  // texel = target - (kLarge - 0.25), so the anchor kLarge is texel 0.25.
  const std::optional<AnchoredTexelRows> rows =
      AnchoredTexelRowsFor(Transform2d::Translate(kLarge - 0.25, 0.0), Vector2i(kLarge, 0));
  ASSERT_THAT(rows, testing::Optional(testing::_));
  EXPECT_EQ(rows->whole, Vector2i(0, 0));
  EXPECT_EQ(rows->fraction[0], 0.25f);
  EXPECT_EQ(rows->anchor, Vector2i(kLarge, 0));
  EXPECT_THAT(AnchoredTexelRowsFor(Transform2d::Translate(-3e9, 0.0), Vector2i::Zero()),
              testing::Eq(std::nullopt));
}

// A fraction just below one that single precision rounds up to one carries into the whole part.
TEST(GeodeAnchoredSampling, FractionRoundingToOneCarries) {
  // texel = target + 5 + (1 - 1e-8): above the snap distance, but 1 - 1e-8 rounds to 1.0f.
  const std::optional<AnchoredTexelRows> rows =
      AnchoredTexelRowsFor(Transform2d::Translate(-(6.0 - 1e-8), 0.0), Vector2i::Zero());
  ASSERT_THAT(rows, testing::Optional(testing::_));
  EXPECT_EQ(rows->whole, Vector2i(6, 0));
  EXPECT_EQ(rows->fraction[0], 0.0f);
}

TEST(GeodeAnchoredSampling, RejectsASingularTransform) {
  EXPECT_THAT(AnchoredTexelRowsFor(Transform2d::Scale(0.0, 1.0), Vector2i::Zero()),
              testing::Eq(std::nullopt));
}

}  // namespace
}  // namespace donner::geode
