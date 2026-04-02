#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "tiny_skia/Edge.h"
#include "tiny_skia/path64/Point64.h"

namespace {

auto Point64Eq(double expectedX, double expectedY) {
  return testing::AllOf(testing::Field(&tiny_skia::Point64::x, testing::DoubleEq(expectedX)),
                        testing::Field(&tiny_skia::Point64::y, testing::DoubleEq(expectedY)));
}

auto PointEq(float expectedX, float expectedY) {
  return testing::AllOf(testing::Field(&tiny_skia::Point::x, testing::FloatEq(expectedX)),
                        testing::Field(&tiny_skia::Point::y, testing::FloatEq(expectedY)));
}

}  // namespace

TEST(Point64Test, FromPointAndToPointRoundTrip) {
  const tiny_skia::Point source{1.25f, -3.5f};
  const auto p = tiny_skia::Point64::fromPoint(source);

  EXPECT_THAT(p, Point64Eq(1.25, -3.5));
  const auto expected = tiny_skia::Point64::fromXY(1.25, -3.5);
  EXPECT_THAT(p, Point64Eq(expected.x, expected.y));

  const auto back = p.toPoint();
  EXPECT_THAT(back, PointEq(source.x, source.y));
}

TEST(Point64Test, SearchAxisCoordinates) {
  const tiny_skia::Point64 p{7.5, -2.5};
  EXPECT_DOUBLE_EQ(p.axisCoord(tiny_skia::SearchAxis::X), 7.5);
  EXPECT_DOUBLE_EQ(p.axisCoord(tiny_skia::SearchAxis::Y), -2.5);
}
