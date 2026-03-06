#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

#include "tiny_skia/path64/Path64.h"

TEST(ModTest, ScalarHelpersBindAndClamp) {
  EXPECT_TRUE(tiny_skia::path64::approximatelyZeroOrMore(0.0));
  EXPECT_TRUE(tiny_skia::path64::between(0.5, 0.0, 1.0));
  EXPECT_TRUE(tiny_skia::path64::between(0.5, 1.0, 0.0));
  EXPECT_DOUBLE_EQ(tiny_skia::path64::bound(2.0, 0.0, 1.0), 1.0);
  EXPECT_DOUBLE_EQ(tiny_skia::path64::bound(-1.0, 0.0, 1.0), 0.0);
  EXPECT_DOUBLE_EQ(tiny_skia::path64::bound(0.5, 0.0, 1.0), 0.5);
}

TEST(ModTest, CubeRootMatchesBasicExpectedValues) {
  EXPECT_DOUBLE_EQ(tiny_skia::path64::cubeRoot(27.0), 3.0);
  EXPECT_DOUBLE_EQ(tiny_skia::path64::cubeRoot(0.0), 0.0);
  EXPECT_DOUBLE_EQ(tiny_skia::path64::cubeRoot(-27.0), -3.0);
}

TEST(ModTest, CubeRootHalleyMethodForNonTrivialValues) {
  // Verify the Halley-method cube root matches std::cbrt to high precision
  // for a range of values (ensures cbrt5d initial estimate + 3 Halley iterations converge).
  EXPECT_NEAR(tiny_skia::path64::cubeRoot(8.0), 2.0, 1e-14);
  EXPECT_NEAR(tiny_skia::path64::cubeRoot(1000.0), 10.0, 1e-12);
  EXPECT_NEAR(tiny_skia::path64::cubeRoot(0.001), 0.1, 1e-14);
  EXPECT_NEAR(tiny_skia::path64::cubeRoot(-8.0), -2.0, 1e-14);
  EXPECT_NEAR(tiny_skia::path64::cubeRoot(1.0), 1.0, 1e-14);
  // Very small value near the approximatelyZeroCubed threshold
  EXPECT_DOUBLE_EQ(tiny_skia::path64::cubeRoot(1e-30), 0.0);
}

TEST(ModTest, ApproximatelyEqualVariantsAndInterpolation) {
  EXPECT_TRUE(tiny_skia::path64::approximatelyEqual(1.0, 1.0 + 1e-16));
  EXPECT_TRUE(tiny_skia::path64::almostDequalUlps(1.0f, 1.0 + 1e-7f));
  EXPECT_DOUBLE_EQ(tiny_skia::path64::interp(2.0, 10.0, 0.25), 4.0);
}
