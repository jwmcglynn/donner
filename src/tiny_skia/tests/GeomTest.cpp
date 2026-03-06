#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

#include "tiny_skia/Geom.h"
#include "tiny_skia/PathRectRs.h"
#include "tiny_skia/PathScalarRs.h"
#include "tiny_skia/PathSizeRs.h"
#include "tiny_skia/tests/test_utils/GeomMatchers.h"

using tiny_skia::tests::matchers::OptionalScreenIntRectEq;
using tiny_skia::tests::matchers::ScreenIntRectEq;

TEST(GeomTest, ScreenIntRectFromXYWHRejectsInvalidDimensions) {
  EXPECT_THAT(tiny_skia::ScreenIntRect::fromXYWH(0, 0, 0, 0), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::ScreenIntRect::fromXYWH(0, 0, 1, 0), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::ScreenIntRect::fromXYWH(0, 0, 0, 1), testing::Eq(std::nullopt));
}

TEST(GeomTest, ScreenIntRectFromXYWHRejectsOverflowAndBounds) {
  const auto max = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
  EXPECT_THAT(
      tiny_skia::ScreenIntRect::fromXYWH(0, 0, max, std::numeric_limits<std::uint32_t>::max()),
      testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::ScreenIntRect::fromXYWH(max, 0, 1, 1), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::ScreenIntRect::fromXYWH(0, max, 1, 1), testing::Eq(std::nullopt));
}

TEST(GeomTest, ScreenIntRectOperations) {
  auto rOpt = tiny_skia::ScreenIntRect::fromXYWH(1, 2, 3, 4);
  ASSERT_THAT(rOpt, testing::Optional(testing::_));
  const auto r = *rOpt;
  EXPECT_THAT(r, ScreenIntRectEq(1u, 2u, 3u, 4u));
  EXPECT_EQ(r.right(), 4u);
  EXPECT_EQ(r.bottom(), 6u);
  EXPECT_TRUE(r.contains(r));
}

TEST(GeomTest, IntSizeAndRectConversions) {
  const auto sizeOpt = tiny_skia::IntSize::fromWH(3, 4);
  ASSERT_THAT(sizeOpt, testing::Optional(testing::_));

  const auto screen = sizeOpt->toScreenIntRect(1, 2);
  EXPECT_THAT(screen, ScreenIntRectEq(1u, 2u, 3u, 4u));

  const auto rectOpt = tiny_skia::IntRect::fromXYWH(10, 20, 3, 4);
  ASSERT_THAT(rectOpt, testing::Optional(testing::_));
  const auto rect = *rectOpt;
  const auto converted = tiny_skia::intRectToScreen(rect);
  EXPECT_THAT(converted, OptionalScreenIntRectEq(10u, 20u, 3u, 4u));
}

TEST(GeomTest, IntSizeFromWhRejectsZero) {
  EXPECT_THAT(tiny_skia::IntSize::fromWH(0, 1), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::IntSize::fromWH(1, 0), testing::Eq(std::nullopt));
}

TEST(GeomTest, IntRectFromXYWHRejectsInvalidInputs) {
  // Negative x/y are valid (IntRect uses int32_t), matching Rust semantics.
  EXPECT_THAT(tiny_skia::IntRect::fromXYWH(-1, 0, 1, 1), testing::Ne(std::nullopt));
  EXPECT_THAT(tiny_skia::IntRect::fromXYWH(0, -1, 1, 1), testing::Ne(std::nullopt));
  // Zero width/height are rejected.
  EXPECT_THAT(tiny_skia::IntRect::fromXYWH(0, 0, 0, 1), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::IntRect::fromXYWH(0, 0, 1, 0), testing::Eq(std::nullopt));
}

TEST(GeomTest, IntRectToScreenIntRectReturnsExpected) {
  const auto rectOpt = tiny_skia::IntRect::fromXYWH(10, 20, 1, 1);
  ASSERT_THAT(rectOpt, testing::Optional(testing::_));
  const auto rect = *rectOpt;
  const auto screen = tiny_skia::intRectToScreen(rect);
  EXPECT_THAT(screen, OptionalScreenIntRectEq(10u, 20u, 1u, 1u));

  const auto direct = rect.toScreenIntRect();
  EXPECT_THAT(direct, OptionalScreenIntRectEq(10u, 20u, 1u, 1u));
}

TEST(GeomTest, RectFromLtrbRejectsInvalidBounds) {
  EXPECT_THAT(tiny_skia::Rect::fromLTRB(2.0f, 1.0f, 1.0f, 2.0f), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::Rect::fromLTRB(1.0f, 2.0f, 1.0f, 1.0f), testing::Eq(std::nullopt));
}

TEST(GeomTest, PathModuleWrappersRouteToUnderlyingImplementations) {
  const auto rectDirect = tiny_skia::Rect::fromXYWH(1.0f, 2.0f, 3.0f, 4.0f);
  const auto rectWrapped = tiny_skia::pathRectRs::fromXYWH(1.0f, 2.0f, 3.0f, 4.0f);
  ASSERT_THAT(rectDirect, testing::Optional(testing::_));
  ASSERT_THAT(rectWrapped, testing::Optional(testing::_));
  EXPECT_EQ(*rectWrapped, *rectDirect);

  const auto sizeDirect = tiny_skia::IntSize::fromWH(3, 4);
  const auto sizeWrapped = tiny_skia::pathSizeRs::fromWH(3, 4);
  ASSERT_THAT(sizeDirect, testing::Optional(testing::_));
  ASSERT_THAT(sizeWrapped, testing::Optional(testing::_));
  EXPECT_EQ(*sizeWrapped, *sizeDirect);

  EXPECT_FLOAT_EQ(tiny_skia::pathScalarRs::half(8.0f), 4.0f);
  EXPECT_FLOAT_EQ(tiny_skia::pathScalarRs::ave(2.0f, 6.0f), 4.0f);
  EXPECT_FLOAT_EQ(tiny_skia::pathScalarRs::bound(2.0f, 0.0f, 1.0f), 1.0f);
}
