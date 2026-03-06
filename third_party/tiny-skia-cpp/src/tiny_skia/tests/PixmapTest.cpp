#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/tests/test_utils/PixmapMaskMatchers.h"

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::Optional;
using tiny_skia::tests::matchers::OptionalMutableSubPixmapViewEq;
using tiny_skia::tests::matchers::PremultipliedColorU8Eq;

TEST(PixmapTest, FromSizeRejectsZeroAndTooWideInputs) {
  EXPECT_THAT(tiny_skia::Pixmap::fromSize(0, 1), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::Pixmap::fromSize(1, 0), testing::Eq(std::nullopt));

  const auto tooWide =
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max() / 4) + 1u;
  EXPECT_THAT(tiny_skia::Pixmap::fromSize(tooWide, 1), testing::Eq(std::nullopt));
}

TEST(PixmapTest, FromSizeAllocatesZeroedRgbaBuffer) {
  const auto pixmapOpt = tiny_skia::Pixmap::fromSize(3, 2);
  ASSERT_THAT(pixmapOpt, Optional(testing::_));
  const auto pixmap = *pixmapOpt;

  EXPECT_EQ(pixmap.width(), 3u);
  EXPECT_EQ(pixmap.height(), 2u);
  ASSERT_EQ(pixmap.data().size(), 24u);
  EXPECT_THAT(pixmap.data(), Each(0u));
}

TEST(PixmapTest, FromVecValidatesExactByteLength) {
  const auto size = tiny_skia::IntSize::fromWH(2, 2);
  ASSERT_THAT(size, Optional(testing::_));

  EXPECT_THAT(tiny_skia::Pixmap::fromVec(std::vector<std::uint8_t>(15, 0), *size),
              testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::Pixmap::fromVec(std::vector<std::uint8_t>(16, 0), *size),
              Optional(testing::_));
}

TEST(PixmapTest, PixmapViewFromBytesAndPixelAccessMatchRgbaPacking) {
  const std::vector<std::uint8_t> bytes{
      1, 2, 3, 4, 5, 6, 7, 8,
  };

  auto refOpt = tiny_skia::PixmapView::fromBytes(bytes, 2, 1);
  ASSERT_THAT(refOpt, Optional(testing::_));
  const auto ref = *refOpt;

  EXPECT_EQ(ref.width(), 2u);
  EXPECT_EQ(ref.height(), 1u);
  ASSERT_EQ(ref.pixels().size(), 2u);
  EXPECT_THAT(ref.pixels()[0], PremultipliedColorU8Eq(1u, 2u, 3u, 4u));
  EXPECT_THAT(ref.pixels()[1], PremultipliedColorU8Eq(5u, 6u, 7u, 8u));

  const auto p = ref.pixel(1, 0);
  ASSERT_THAT(p, Optional(testing::_));
  EXPECT_THAT(*p, PremultipliedColorU8Eq(5u, 6u, 7u, 8u));
  EXPECT_THAT(ref.pixel(2, 0), testing::Eq(std::nullopt));
}

TEST(PixmapTest, DataMutPixelsMutAndTakeExposeOwnedStorage) {
  auto pixmapOpt = tiny_skia::Pixmap::fromSize(1, 1);
  ASSERT_THAT(pixmapOpt, Optional(testing::_));
  auto pixmap = std::move(*pixmapOpt);

  auto mutableBytes = pixmap.data();
  mutableBytes[0] = 9;
  mutableBytes[1] = 10;
  mutableBytes[2] = 11;
  mutableBytes[3] = 12;

  ASSERT_EQ(pixmap.pixels().size(), 1u);
  EXPECT_THAT(pixmap.pixels()[0], PremultipliedColorU8Eq(9u, 10u, 11u, 12u));

  const auto taken = pixmap.release();
  ASSERT_EQ(taken.size(), 4u);
  EXPECT_THAT(taken, ElementsAre(9u, 10u, 11u, 12u));
  EXPECT_TRUE(pixmap.data().empty());
  EXPECT_EQ(pixmap.width(), 0u);
  EXPECT_EQ(pixmap.height(), 0u);
}

TEST(PixmapTest, FillAndTakeDemultipliedMatchColorConversion) {
  auto pixmapOpt = tiny_skia::Pixmap::fromSize(2, 1);
  ASSERT_THAT(pixmapOpt, Optional(testing::_));
  auto pixmap = std::move(*pixmapOpt);

  const auto color = tiny_skia::Color::fromRgba8(255, 0, 0, 128);
  pixmap.fill(color);
  ASSERT_EQ(pixmap.pixels().size(), 2u);
  EXPECT_THAT(pixmap.pixels()[0], PremultipliedColorU8Eq(128u, 0u, 0u, 128u));

  const auto demul = pixmap.releaseDemultiplied();
  ASSERT_EQ(demul.size(), 8u);
  EXPECT_THAT(demul, ElementsAre(255u, 0u, 0u, 128u, 255u, 0u, 0u, 128u));
}

TEST(PixmapTest, CloneRectCopiesContainedRegion) {
  const auto size = tiny_skia::IntSize::fromWH(3, 2);
  ASSERT_THAT(size, Optional(testing::_));
  auto pixmap = tiny_skia::Pixmap::fromVec(
      std::vector<std::uint8_t>{
          1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
      },
      *size);
  ASSERT_THAT(pixmap, Optional(testing::_));
  const auto rect = tiny_skia::IntRect::fromXYWH(1, 0, 2, 2);
  ASSERT_THAT(rect, Optional(testing::_));

  const auto cloned = pixmap->cloneRect(*rect);
  ASSERT_THAT(cloned, Optional(testing::_));
  EXPECT_EQ(cloned->width(), 2u);
  EXPECT_EQ(cloned->height(), 2u);
  ASSERT_EQ(cloned->data().size(), 16u);
  EXPECT_THAT(cloned->data(), ElementsAre(5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 17u, 18u, 19u, 20u,
                                          21u, 22u, 23u, 24u));
}

TEST(PixmapTest, MutablePixmapViewFromBytesAndSubpixmapProvideMutableSubview) {
  std::vector<std::uint8_t> bytes{
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
  };
  auto mut = tiny_skia::MutablePixmapView::fromBytes(bytes, 2, 2);
  ASSERT_THAT(mut, Optional(testing::_));
  ASSERT_EQ(mut->pixels().size(), 4u);
  EXPECT_THAT(mut->pixels()[0], PremultipliedColorU8Eq(1u, 2u, 3u, 4u));

  const auto rect = tiny_skia::IntRect::fromXYWH(1, 1, 1, 1);
  ASSERT_THAT(rect, Optional(testing::_));
  const auto sub = mut->subpixmap(*rect);
  ASSERT_THAT(sub, OptionalMutableSubPixmapViewEq(1u, 1u, 2u));
  EXPECT_EQ(sub->data[0], 13u);
  sub->data[0] = 99u;
  EXPECT_EQ(bytes[12], 99u);
}
