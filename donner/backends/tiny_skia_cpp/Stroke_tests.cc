#include "donner/backends/tiny_skia_cpp/Stroke.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>

namespace donner::backends::tiny_skia_cpp {
namespace {

using ::testing::FloatNear;

TEST(StrokeTests, DefaultsMatchTinySkia) {
  Stroke stroke;
  EXPECT_FLOAT_EQ(stroke.width, 1.0f);
  EXPECT_FLOAT_EQ(stroke.miterLimit, 4.0f);
  EXPECT_EQ(stroke.lineCap, LineCap::kButt);
  EXPECT_EQ(stroke.lineJoin, LineJoin::kMiter);
  EXPECT_FALSE(stroke.dash.has_value());
}

TEST(StrokeDashTests, RejectsInvalidPatterns) {
  EXPECT_FALSE(StrokeDash::Create({}, 0.0f).has_value());
  EXPECT_FALSE(StrokeDash::Create({1.0f}, 0.0f).has_value());
  EXPECT_FALSE(StrokeDash::Create({1.0f, 2.0f, 3.0f}, 0.0f).has_value());
  EXPECT_FALSE(StrokeDash::Create({1.0f, -2.0f}, 0.0f).has_value());
  EXPECT_FALSE(StrokeDash::Create({0.0f, 0.0f}, 0.0f).has_value());
  EXPECT_FALSE(StrokeDash::Create({1.0f, -1.0f}, 0.0f).has_value());
  EXPECT_FALSE(
      StrokeDash::Create({1.0f, 1.0f}, std::numeric_limits<float>::infinity()).has_value());
  EXPECT_FALSE(
      StrokeDash::Create({1.0f, std::numeric_limits<float>::infinity()}, 0.0f).has_value());
}

TEST(StrokeDashTests, NormalizesOffsetAndFindsFirstInterval) {
  auto dash = StrokeDash::Create({6.0f, 4.5f}, -2.5f);
  ASSERT_TRUE(dash.has_value());
  EXPECT_FLOAT_EQ(dash->intervalLength(), 10.5f);
  EXPECT_THAT(dash->offset(), FloatNear(8.0f, 1e-5f));
  EXPECT_THAT(dash->firstLength(), FloatNear(2.5f, 1e-5f));
  EXPECT_EQ(dash->firstIndex(), 1u);

  auto wrapped = StrokeDash::Create({3.0f, 1.0f, 2.0f, 4.0f}, 17.0f);
  ASSERT_TRUE(wrapped.has_value());
  EXPECT_FLOAT_EQ(wrapped->intervalLength(), 10.0f);
  EXPECT_THAT(wrapped->offset(), FloatNear(7.0f, 1e-5f));
  EXPECT_THAT(wrapped->firstLength(), FloatNear(3.0f, 1e-5f));
  EXPECT_EQ(wrapped->firstIndex(), 3u);
}

}  // namespace
}  // namespace donner::backends::tiny_skia_cpp
