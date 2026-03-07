#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

#include "tiny_skia/Geom.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/pipeline/Pipeline.h"
#include "tiny_skia/tests/test_utils/PixmapMaskMatchers.h"

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Optional;
using tiny_skia::tests::matchers::OptionalMutableSubMaskViewEq;
using tiny_skia::tests::matchers::OptionalSubMaskViewEq;

TEST(MaskTest, FromSizeRejectsZeroDimensions) {
  EXPECT_THAT(tiny_skia::Mask::fromSize(0, 1), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::Mask::fromSize(1, 0), testing::Eq(std::nullopt));
}

TEST(MaskTest, FromSizeInitializesZeroedBufferAndDimensions) {
  const auto maskOpt = tiny_skia::Mask::fromSize(3, 2);
  ASSERT_THAT(maskOpt, Optional(testing::_));
  const auto mask = *maskOpt;

  EXPECT_EQ(mask.width(), 3u);
  EXPECT_EQ(mask.height(), 2u);
  EXPECT_EQ(mask.data().size(), 6u);
  EXPECT_THAT(mask.data(), Each(0u));
}

TEST(MaskTest, FromVecRequiresExactSize) {
  const auto size = tiny_skia::IntSize::fromWH(2, 2);
  ASSERT_THAT(size, Optional(testing::_));

  EXPECT_THAT(tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{1, 2, 3}, *size),
              testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{1, 2, 3, 4}, *size),
              Optional(testing::_));
}

TEST(MaskTest, DataMutAndTakeExposeOwnedBuffer) {
  auto maskOpt = tiny_skia::Mask::fromSize(2, 2);
  ASSERT_THAT(maskOpt, Optional(testing::_));
  auto mask = std::move(*maskOpt);

  auto writable = mask.data();
  writable[0] = 7;
  writable[3] = 11;
  EXPECT_EQ(mask.data()[0], 7u);
  EXPECT_EQ(mask.data()[3], 11u);

  const auto taken = mask.release();
  ASSERT_EQ(taken.size(), 4u);
  EXPECT_THAT(taken, ElementsAre(7u, 0u, 0u, 11u));
  EXPECT_TRUE(mask.data().empty());
  EXPECT_EQ(mask.width(), 0u);
  EXPECT_EQ(mask.height(), 0u);
}

TEST(MaskTest, FromPixmapAlphaCopiesAlphaChannel) {
  const auto size = tiny_skia::IntSize::fromWH(2, 1);
  ASSERT_THAT(size, Optional(testing::_));
  const auto pixmap =
      tiny_skia::Pixmap::fromVec(std::vector<std::uint8_t>{10, 20, 30, 40, 50, 60, 70, 80}, *size);
  ASSERT_THAT(pixmap, Optional(testing::_));

  const auto mask = tiny_skia::Mask::fromPixmap(pixmap->view(), tiny_skia::MaskType::Alpha);
  ASSERT_EQ(mask.data().size(), 2u);
  EXPECT_THAT(mask.data(), ElementsAre(40u, 80u));
}

TEST(MaskTest, FromPixmapLuminanceUsesDemultiplyThenLumaTimesAlpha) {
  const auto size = tiny_skia::IntSize::fromWH(3, 1);
  ASSERT_THAT(size, Optional(testing::_));
  const auto pixmap = tiny_skia::Pixmap::fromVec(
      std::vector<std::uint8_t>{
          255,
          0,
          0,
          255,
          64,
          0,
          0,
          128,
          200,
          100,
          50,
          0,
      },
      *size);
  ASSERT_THAT(pixmap, Optional(testing::_));

  const auto mask = tiny_skia::Mask::fromPixmap(pixmap->view(), tiny_skia::MaskType::Luminance);
  ASSERT_EQ(mask.data().size(), 3u);
  EXPECT_THAT(mask.data(), ElementsAre(55u, 14u, 0u));
}

TEST(MaskTest, SubmaskComputesIntersectedViewAndOffset) {
  auto maskOpt = tiny_skia::Mask::fromSize(4, 3);
  ASSERT_THAT(maskOpt, Optional(testing::_));
  auto mask = std::move(*maskOpt);
  auto bytes = mask.data();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i);
  }

  const auto rect = tiny_skia::IntRect::fromXYWH(1, 1, 2, 1);
  ASSERT_THAT(rect, Optional(testing::_));
  const auto sub = mask.submask(*rect);
  ASSERT_THAT(sub, OptionalSubMaskViewEq(2u, 1u, 4u));
  const std::span<const std::uint8_t> subBytes(sub->data, 2);
  EXPECT_THAT(subBytes, ElementsAre(5u, 6u));
}

TEST(MaskTest, SubpixmapComputesIntersectedMutableView) {
  auto maskOpt = tiny_skia::Mask::fromSize(4, 3);
  ASSERT_THAT(maskOpt, Optional(testing::_));
  auto mask = std::move(*maskOpt);
  auto bytes = mask.data();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i);
  }

  const auto rect = tiny_skia::IntRect::fromXYWH(3, 2, 3, 2);
  ASSERT_THAT(rect, Optional(testing::_));
  const auto sub = mask.subpixmap(*rect);
  ASSERT_THAT(sub, OptionalMutableSubMaskViewEq(1u, 1u, 4u));
  EXPECT_EQ(sub->data[0], 11u);

  sub->data[0] = 99u;
  EXPECT_EQ(mask.data()[11], 99u);
}

// ---- invert tests ----

TEST(MaskTest, InvertFlipsAllBytes) {
  auto mask = tiny_skia::Mask::fromSize(3, 1);
  ASSERT_THAT(mask, Optional(testing::_));
  auto data = mask->data();
  data[0] = 0;
  data[1] = 128;
  data[2] = 255;

  mask->invert();
  EXPECT_THAT(mask->data(), ElementsAre(255u, 127u, 0u));
}

// ---- clear tests ----

TEST(MaskTest, ClearZerosAllData) {
  auto mask = tiny_skia::Mask::fromSize(2, 2);
  ASSERT_THAT(mask, Optional(testing::_));
  auto data = mask->data();
  data[0] = 10;
  data[1] = 20;
  data[2] = 30;
  data[3] = 40;

  mask->clear();
  EXPECT_THAT(mask->data(), Each(0u));
}

// ---- fillPath tests ----

TEST(MaskTest, FillPathDrawsOntoMask) {
  auto mask = tiny_skia::Mask::fromSize(10, 10);
  ASSERT_THAT(mask, Optional(testing::_));

  // Build a rectangular path covering part of the mask.
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  builder.lineTo(8.0f, 2.0f);
  builder.lineTo(8.0f, 8.0f);
  builder.lineTo(2.0f, 8.0f);
  builder.close();
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  mask->fillPath(*path, tiny_skia::FillRule::Winding, false, tiny_skia::Transform::identity());

  // Interior pixels should be non-zero (filled).
  // Check a pixel well inside the rectangle.
  const auto rowStride = mask->width();
  // Row 5, col 5 should be inside the filled rect.
  EXPECT_GT(mask->data()[5 * rowStride + 5], 0u);
  // Corner at (0,0) should still be zero (outside the path).
  EXPECT_EQ(mask->data()[0], 0u);
}

TEST(MaskTest, FillPathWithTransformOffsetsPath) {
  auto mask = tiny_skia::Mask::fromSize(10, 10);
  ASSERT_THAT(mask, Optional(testing::_));

  tiny_skia::PathBuilder builder;
  builder.moveTo(0.0f, 0.0f);
  builder.lineTo(4.0f, 0.0f);
  builder.lineTo(4.0f, 4.0f);
  builder.lineTo(0.0f, 4.0f);
  builder.close();
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  // Translate path to center of mask.
  auto ts = tiny_skia::Transform::fromTranslate(3.0f, 3.0f);
  mask->fillPath(*path, tiny_skia::FillRule::Winding, false, ts);

  // (0,0) should be zero (path was translated away).
  EXPECT_EQ(mask->data()[0], 0u);
  // (5,5) should be inside the translated rect (3..7, 3..7).
  EXPECT_GT(mask->data()[5 * mask->width() + 5], 0u);
}

TEST(MaskTest, FillPathAaUses255BasedCoverageForThreeQuarterPixel) {
  auto mask = tiny_skia::Mask::fromSize(5, 5);
  ASSERT_THAT(mask, Optional(testing::_));

  tiny_skia::PathBuilder builder;
  builder.moveTo(1.0f, 1.25f);
  builder.lineTo(3.0f, 1.25f);
  builder.lineTo(3.0f, 3.0f);
  builder.lineTo(1.0f, 3.0f);
  builder.close();
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  mask->fillPath(*path, tiny_skia::FillRule::Winding, true, tiny_skia::Transform::identity());

  const auto rowStride = mask->width();
  EXPECT_EQ(mask->data()[1 * rowStride + 1], 192u);
  EXPECT_EQ(mask->data()[1 * rowStride + 2], 192u);
  EXPECT_EQ(mask->data()[2 * rowStride + 1], 255u);
  EXPECT_EQ(mask->data()[2 * rowStride + 2], 255u);
}

TEST(MaskTest, FillPathAaUses255BasedCoverageForFifteenSixteenthPixel) {
  auto mask = tiny_skia::Mask::fromSize(3, 3);
  ASSERT_THAT(mask, Optional(testing::_));

  tiny_skia::PathBuilder builder;
  builder.moveTo(0.0625f, 0.0f);
  builder.lineTo(1.0f, 0.0f);
  builder.lineTo(1.0f, 1.0f);
  builder.lineTo(0.0625f, 1.0f);
  builder.close();
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  mask->fillPath(*path, tiny_skia::FillRule::Winding, true, tiny_skia::Transform::identity());

  const auto rowStride = mask->width();
  EXPECT_EQ(mask->data()[0 * rowStride + 0], 240u);
  EXPECT_EQ(mask->data()[0 * rowStride + 1], 0u);
  EXPECT_EQ(mask->data()[1 * rowStride + 0], 0u);
}

// ---- intersectPath tests ----

TEST(MaskTest, IntersectPathMultipliesMasks) {
  auto mask = tiny_skia::Mask::fromSize(10, 10);
  ASSERT_THAT(mask, Optional(testing::_));
  // Fill entire mask with 255.
  std::fill(mask->data().begin(), mask->data().end(), static_cast<std::uint8_t>(255));

  // Intersect with a small rect: only the rect area should remain.
  tiny_skia::PathBuilder builder;
  builder.moveTo(2.0f, 2.0f);
  builder.lineTo(5.0f, 2.0f);
  builder.lineTo(5.0f, 5.0f);
  builder.lineTo(2.0f, 5.0f);
  builder.close();
  auto path = builder.finish();
  ASSERT_TRUE(path.has_value());

  mask->intersectPath(*path, tiny_skia::FillRule::Winding, false, tiny_skia::Transform::identity());

  // Outside the rect should be 0 (255 * 0 / 255 = 0).
  EXPECT_EQ(mask->data()[0], 0u);
  // Inside should remain 255 (255 * 255 / 255 = 255).
  EXPECT_EQ(mask->data()[3 * mask->width() + 3], 255u);
}
