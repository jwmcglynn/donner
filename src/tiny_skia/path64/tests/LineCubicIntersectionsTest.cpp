#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

#include "tiny_skia/path64/Cubic64.h"
#include "tiny_skia/path64/LineCubicIntersections.h"

TEST(LineCubicIntersectionsTest, HorizontalIntersectFindsExpectedSingleRoot) {
  const auto cubic = tiny_skia::path64::cubic64::Cubic64::create({
      tiny_skia::Point64::fromXY(0.0, 0.0),
      tiny_skia::Point64::fromXY(0.3333333333333333, 1.0 / 3.0),
      tiny_skia::Point64::fromXY(0.6666666666666666, 2.0 / 3.0),
      tiny_skia::Point64::fromXY(1.0, 1.0),
  });

  std::array<double, 3> roots{};
  const auto count =
      tiny_skia::path64::line_cubic_intersections::horizontalIntersect(cubic, 0.5, roots);
  EXPECT_EQ(count, 1u);
  EXPECT_THAT(roots[0], testing::DoubleEq(0.5));
}

TEST(LineCubicIntersectionsTest, VerticalIntersectFindsExpectedSingleRoot) {
  const auto cubic = tiny_skia::path64::cubic64::Cubic64::create({
      tiny_skia::Point64::fromXY(0.0, 0.0),
      tiny_skia::Point64::fromXY(1.0 / 3.0, 0.0),
      tiny_skia::Point64::fromXY(2.0 / 3.0, 0.0),
      tiny_skia::Point64::fromXY(1.0, 0.0),
  });

  std::array<double, 3> roots{};
  const auto count =
      tiny_skia::path64::line_cubic_intersections::verticalIntersect(cubic, 0.5, roots);
  EXPECT_EQ(count, 1u);
  EXPECT_THAT(roots[0], testing::DoubleEq(0.5));
}

TEST(LineCubicIntersectionsTest, VerticalIntersectReturnsZeroForMiss) {
  const auto cubic = tiny_skia::path64::cubic64::Cubic64::create({
      tiny_skia::Point64::fromXY(0.0, 0.0),
      tiny_skia::Point64::fromXY(0.25, 0.0),
      tiny_skia::Point64::fromXY(0.5, 0.0),
      tiny_skia::Point64::fromXY(1.0, 0.0),
  });

  std::array<double, 3> roots{};
  const auto count =
      tiny_skia::path64::line_cubic_intersections::verticalIntersect(cubic, 2.0, roots);
  EXPECT_EQ(count, 0u);
}

// Regression: horizontalIntersect must pass y-coordinates (offset by 1) to coefficients,
// not the full x,y coordinate slice. This cubic has different x and y behavior, so using
// x-coordinates would give wrong results.
TEST(LineCubicIntersectionsTest, HorizontalIntersectUsesYCoordinates) {
  // Cubic where x goes 0->10 linearly but y goes 0->1 as a cubic.
  // horizontalIntersect at y=0.5 should find a root.
  const auto cubic = tiny_skia::path64::cubic64::Cubic64::create({
      tiny_skia::Point64::fromXY(0.0, 0.0),
      tiny_skia::Point64::fromXY(3.0, 0.0),
      tiny_skia::Point64::fromXY(7.0, 1.0),
      tiny_skia::Point64::fromXY(10.0, 1.0),
  });

  std::array<double, 3> roots{};
  const auto count =
      tiny_skia::path64::line_cubic_intersections::horizontalIntersect(cubic, 0.5, roots);
  EXPECT_GE(count, 1u);
  // Verify the found root maps to approximately y=0.5.
  const auto pt = cubic.pointAtT(roots[0]);
  EXPECT_NEAR(pt.y, 0.5, 1e-6);
}
