#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <span>

#include "tiny_skia/Geom.h"
#include "tiny_skia/LineClipper.h"

namespace {

auto PointEq(float expectedX, float expectedY) {
  return testing::AllOf(testing::Field(&tiny_skia::Point::x, testing::FloatEq(expectedX)),
                        testing::Field(&tiny_skia::Point::y, testing::FloatEq(expectedY)));
}

}  // namespace

TEST(LineClipperTest, ClipRejectsFullyAboveAndBelow) {
  const auto clip = tiny_skia::Rect::fromLTRB(0.0f, 0.0f, 10.0f, 10.0f).value();
  std::array<tiny_skia::Point, 2> src{{{0.0f, -5.0f}, {5.0f, -1.0f}}};
  std::array<tiny_skia::Point, tiny_skia::lineClipper::kLineClipperMaxPoints> out{};

  auto noClip = tiny_skia::lineClipper::clip(std::span<const tiny_skia::Point, 2>(src), clip,
                                              false, std::span<tiny_skia::Point, 4>(out));
  EXPECT_TRUE(noClip.empty());

  std::array<tiny_skia::Point, 2> belowSrc{{{0.0f, 12.0f}, {5.0f, 20.0f}}};
  auto noClipBelow =
      tiny_skia::lineClipper::clip(std::span<const tiny_skia::Point, 2>(belowSrc), clip, false,
                                    std::span<tiny_skia::Point, 4>(out));
  EXPECT_TRUE(noClipBelow.empty());
}

TEST(LineClipperTest, ClipClampsHorizontallyInsideBounds) {
  const auto clip = tiny_skia::Rect::fromLTRB(1.0f, 0.0f, 9.0f, 10.0f).value();
  std::array<tiny_skia::Point, 2> src{{{-5.0f, 3.0f}, {5.0f, 3.0f}}};
  std::array<tiny_skia::Point, tiny_skia::lineClipper::kLineClipperMaxPoints> out{};

  auto result = tiny_skia::lineClipper::clip(std::span<const tiny_skia::Point, 2>(src), clip,
                                              false, std::span<tiny_skia::Point, 4>(out));
  EXPECT_THAT(result,
              testing::ElementsAre(PointEq(1.0f, 3.0f), PointEq(1.0f, 3.0f), PointEq(5.0f, 3.0f)));
}

TEST(LineClipperTest, ClipClampsBothSidesOnSkewLine) {
  const auto clip = tiny_skia::Rect::fromLTRB(0.0f, 0.0f, 10.0f, 10.0f).value();
  std::array<tiny_skia::Point, 2> src{{{-5.0f, 2.0f}, {15.0f, 8.0f}}};
  std::array<tiny_skia::Point, tiny_skia::lineClipper::kLineClipperMaxPoints> out{};

  auto result = tiny_skia::lineClipper::clip(std::span<const tiny_skia::Point, 2>(src), clip,
                                              false, std::span<tiny_skia::Point, 4>(out));
  EXPECT_THAT(result, testing::ElementsAre(PointEq(0.0f, 2.0f), PointEq(0.0f, 3.5f),
                                           PointEq(10.0f, 6.5f), PointEq(10.0f, 8.0f)));
}

TEST(LineClipperTest, ClipCanCullToRightAndPreserveWhenFalse) {
  const auto clip = tiny_skia::Rect::fromLTRB(1.0f, 0.0f, 10.0f, 10.0f).value();
  std::array<tiny_skia::Point, 2> src{{{12.0f, 2.0f}, {20.0f, 4.0f}}};
  std::array<tiny_skia::Point, tiny_skia::lineClipper::kLineClipperMaxPoints> out{};

  auto culled = tiny_skia::lineClipper::clip(std::span<const tiny_skia::Point, 2>(src), clip, true,
                                              std::span<tiny_skia::Point, 4>(out));
  EXPECT_TRUE(culled.empty());

  auto preserved = tiny_skia::lineClipper::clip(std::span<const tiny_skia::Point, 2>(src), clip,
                                                 false, std::span<tiny_skia::Point, 4>(out));
  EXPECT_THAT(preserved, testing::ElementsAre(PointEq(10.0f, 2.0f), PointEq(10.0f, 4.0f)));
}

TEST(LineClipperTest, IntersectClipsAndReturnsTrueForPartiallyOverlapping) {
  const auto clip = tiny_skia::Rect::fromLTRB(1.0f, 1.0f, 9.0f, 9.0f).value();
  std::array<tiny_skia::Point, 2> src{{{-5.0f, -5.0f}, {15.0f, 15.0f}}};
  std::array<tiny_skia::Point, 2> dst{};

  auto intersects = tiny_skia::lineClipper::intersect(std::span<const tiny_skia::Point, 2>(src),
                                                       clip, std::span<tiny_skia::Point, 2>(dst));
  EXPECT_TRUE(intersects);
  EXPECT_THAT(dst, testing::ElementsAre(PointEq(1.0f, 1.0f), PointEq(9.0f, 9.0f)));
}

TEST(LineClipperTest, IntersectRejectsDisjointSegment) {
  const auto clip = tiny_skia::Rect::fromLTRB(1.0f, 1.0f, 9.0f, 9.0f).value();
  std::array<tiny_skia::Point, 2> src{{{-5.0f, 3.0f}, {-1.0f, 7.0f}}};
  std::array<tiny_skia::Point, 2> dst{};

  auto intersects = tiny_skia::lineClipper::intersect(std::span<const tiny_skia::Point, 2>(src),
                                                       clip, std::span<tiny_skia::Point, 2>(dst));
  EXPECT_FALSE(intersects);
}
