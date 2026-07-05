#include "donner/base/Length.h"

#include <gtest/gtest.h>

#include <compare>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner {

TEST(LengthTest, DefaultConstructor) {
  Lengthd length;
  EXPECT_EQ(length.value, 0.0);
  EXPECT_EQ(length.unit, Lengthd::Unit::None);
}

TEST(LengthTest, ConstructorWithValueAndUnit) {
  Lengthd length(10.0, Lengthd::Unit::Px);
  EXPECT_EQ(length.value, 10.0);
  EXPECT_EQ(length.unit, Lengthd::Unit::Px);
}

TEST(LengthTest, EqualityOperator) {
  Lengthd length1(10.0, Lengthd::Unit::Px);
  Lengthd length2(10.0, Lengthd::Unit::Px);
  Lengthd length3(10.0, Lengthd::Unit::Em);

  EXPECT_EQ(length1, length2);
  EXPECT_NE(length1, length3);
}

TEST(LengthTest, LessThanOperator) {
  Lengthd length1(10.0, Lengthd::Unit::Px);
  Lengthd length2(20.0, Lengthd::Unit::Px);
  Lengthd length3(10.0, Lengthd::Unit::Em);

  EXPECT_LT(length1, length2);
  EXPECT_GT(length2, length1);
  EXPECT_LT(length1, length3);
  EXPECT_GT(length3, length1);
}

TEST(LengthTest, IsAbsoluteSize) {
  Lengthd length1(10.0, Lengthd::Unit::Px);
  Lengthd length2(10.0, Lengthd::Unit::Percent);
  Lengthd length3(10.0, Lengthd::Unit::Em);

  EXPECT_TRUE(length1.isAbsoluteSize());
  EXPECT_FALSE(length2.isAbsoluteSize());
  EXPECT_FALSE(length3.isAbsoluteSize());
}

TEST(LengthTest, IsAbsoluteSizeCoversAllAbsoluteUnits) {
  EXPECT_TRUE(Lengthd(1.0, Lengthd::Unit::None).isAbsoluteSize());
  EXPECT_TRUE(Lengthd(1.0, Lengthd::Unit::Cm).isAbsoluteSize());
  EXPECT_TRUE(Lengthd(1.0, Lengthd::Unit::Mm).isAbsoluteSize());
  EXPECT_TRUE(Lengthd(1.0, Lengthd::Unit::Q).isAbsoluteSize());
  EXPECT_TRUE(Lengthd(1.0, Lengthd::Unit::In).isAbsoluteSize());
  EXPECT_TRUE(Lengthd(1.0, Lengthd::Unit::Pc).isAbsoluteSize());
  EXPECT_TRUE(Lengthd(1.0, Lengthd::Unit::Pt).isAbsoluteSize());
  EXPECT_TRUE(Lengthd(1.0, Lengthd::Unit::Px).isAbsoluteSize());

  EXPECT_FALSE(Lengthd(1.0, Lengthd::Unit::Percent).isAbsoluteSize());
  EXPECT_FALSE(Lengthd(1.0, Lengthd::Unit::Em).isAbsoluteSize());
  EXPECT_FALSE(Lengthd(1.0, Lengthd::Unit::Vw).isAbsoluteSize());
}

TEST(LengthTest, SpaceshipTreatsNearEqualValuesAsEquivalent) {
  const std::partial_ordering result =
      Lengthd(1.0, Lengthd::Unit::Px) <=> Lengthd(1.0 + 0.5e-16, Lengthd::Unit::Px);
  EXPECT_EQ(result, std::partial_ordering::equivalent);
}

TEST(LengthTest, ToPixelsCoversAbsoluteUnits) {
  const Box2d viewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0);
  const FontMetrics fontMetrics;

  EXPECT_DOUBLE_EQ(Lengthd(12.0).toPixels(viewBox, fontMetrics), 12.0);
  EXPECT_DOUBLE_EQ(Lengthd(2.0, Lengthd::Unit::Cm).toPixels(viewBox, fontMetrics),
                   2.0 * AbsoluteLengthMetrics::kCmToPixels);
  EXPECT_DOUBLE_EQ(Lengthd(20.0, Lengthd::Unit::Mm).toPixels(viewBox, fontMetrics),
                   2.0 * AbsoluteLengthMetrics::kCmToPixels);
  EXPECT_DOUBLE_EQ(Lengthd(40.0, Lengthd::Unit::Q).toPixels(viewBox, fontMetrics),
                   AbsoluteLengthMetrics::kCmToPixels);
  EXPECT_DOUBLE_EQ(Lengthd(1.5, Lengthd::Unit::In).toPixels(viewBox, fontMetrics), 144.0);
  EXPECT_DOUBLE_EQ(Lengthd(6.0, Lengthd::Unit::Pc).toPixels(viewBox, fontMetrics), 96.0);
  EXPECT_DOUBLE_EQ(Lengthd(72.0, Lengthd::Unit::Pt).toPixels(viewBox, fontMetrics), 96.0);
  EXPECT_DOUBLE_EQ(Lengthd(9.0, Lengthd::Unit::Px).toPixels(viewBox, fontMetrics), 9.0);
}

TEST(LengthTest, ToPixelsCoversPercentExtents) {
  const Box2d viewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0);
  const FontMetrics fontMetrics;

  EXPECT_DOUBLE_EQ(
      Lengthd(25.0, Lengthd::Unit::Percent).toPixels(viewBox, fontMetrics, Lengthd::Extent::X),
      50.0);
  EXPECT_DOUBLE_EQ(
      Lengthd(25.0, Lengthd::Unit::Percent).toPixels(viewBox, fontMetrics, Lengthd::Extent::Y),
      25.0);
  EXPECT_DOUBLE_EQ(
      Lengthd(25.0, Lengthd::Unit::Percent).toPixels(viewBox, fontMetrics, Lengthd::Extent::Mixed),
      25.0 * std::sqrt(200.0 * 200.0 + 100.0 * 100.0) / std::sqrt(2.0) / 100.0);
}

TEST(LengthTest, ToPixelsCoversFontRelativeUnits) {
  const Box2d viewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0);
  FontMetrics fontMetrics;
  fontMetrics.fontSize = 20.0;
  fontMetrics.rootFontSize = 18.0;
  fontMetrics.exUnitInEm = 0.4;
  fontMetrics.chUnitInEm = 0.6;

  EXPECT_DOUBLE_EQ(Lengthd(2.0, Lengthd::Unit::Em).toPixels(viewBox, fontMetrics), 40.0);
  EXPECT_DOUBLE_EQ(Lengthd(2.0, Lengthd::Unit::Ex).toPixels(viewBox, fontMetrics), 16.0);
  EXPECT_DOUBLE_EQ(Lengthd(2.0, Lengthd::Unit::Ch).toPixels(viewBox, fontMetrics), 24.0);
  EXPECT_DOUBLE_EQ(Lengthd(2.0, Lengthd::Unit::Rem).toPixels(viewBox, fontMetrics), 36.0);
}

TEST(LengthTest, ToPixelsCoversViewportUnitsWithoutExplicitViewport) {
  const Box2d viewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0);
  const FontMetrics fontMetrics;

  EXPECT_DOUBLE_EQ(Lengthd(10.0, Lengthd::Unit::Vw).toPixels(viewBox, fontMetrics), 20.0);
  EXPECT_DOUBLE_EQ(Lengthd(10.0, Lengthd::Unit::Vh).toPixels(viewBox, fontMetrics), 10.0);
  EXPECT_DOUBLE_EQ(Lengthd(10.0, Lengthd::Unit::Vmin).toPixels(viewBox, fontMetrics), 10.0);
  EXPECT_DOUBLE_EQ(Lengthd(10.0, Lengthd::Unit::Vmax).toPixels(viewBox, fontMetrics), 20.0);
}

TEST(LengthTest, ToPixelsCoversViewportUnitsWithExplicitViewport) {
  const Box2d viewBox = Box2d::FromXYWH(0.0, 0.0, 200.0, 100.0);
  FontMetrics fontMetrics;
  fontMetrics.viewportSize = Vector2d(80.0, 160.0);

  EXPECT_DOUBLE_EQ(Lengthd(10.0, Lengthd::Unit::Vw).toPixels(viewBox, fontMetrics), 8.0);
  EXPECT_DOUBLE_EQ(Lengthd(10.0, Lengthd::Unit::Vh).toPixels(viewBox, fontMetrics), 16.0);
  EXPECT_DOUBLE_EQ(Lengthd(10.0, Lengthd::Unit::Vmin).toPixels(viewBox, fontMetrics), 8.0);
  EXPECT_DOUBLE_EQ(Lengthd(10.0, Lengthd::Unit::Vmax).toPixels(viewBox, fontMetrics), 16.0);
}

/// @test Ostream output \c operator<< for all \ref LengthUnit values.
TEST(LengthTest, LengthUnitOstreamOutput) {
  EXPECT_THAT(Lengthd::Unit::None, ToStringIs(""));
  EXPECT_THAT(Lengthd::Unit::Percent, ToStringIs("%"));
  EXPECT_THAT(Lengthd::Unit::Cm, ToStringIs("cm"));
  EXPECT_THAT(Lengthd::Unit::Mm, ToStringIs("mm"));
  EXPECT_THAT(Lengthd::Unit::Q, ToStringIs("q"));
  EXPECT_THAT(Lengthd::Unit::In, ToStringIs("in"));
  EXPECT_THAT(Lengthd::Unit::Pc, ToStringIs("pc"));
  EXPECT_THAT(Lengthd::Unit::Pt, ToStringIs("pt"));
  EXPECT_THAT(Lengthd::Unit::Px, ToStringIs("px"));
  EXPECT_THAT(Lengthd::Unit::Em, ToStringIs("em"));
  EXPECT_THAT(Lengthd::Unit::Ex, ToStringIs("ex"));
  EXPECT_THAT(Lengthd::Unit::Ch, ToStringIs("ch"));
  EXPECT_THAT(Lengthd::Unit::Rem, ToStringIs("rem"));
  EXPECT_THAT(Lengthd::Unit::Vw, ToStringIs("vw"));
  EXPECT_THAT(Lengthd::Unit::Vh, ToStringIs("vh"));
  EXPECT_THAT(Lengthd::Unit::Vmin, ToStringIs("vmin"));
  EXPECT_THAT(Lengthd::Unit::Vmax, ToStringIs("vmax"));
}

TEST(LengthTest, ToRcStringIntegerValuesOmitDecimal) {
  EXPECT_EQ(Lengthd(10.0, Lengthd::Unit::Px).toRcString(), "10px");
  EXPECT_EQ(Lengthd(0.0).toRcString(), "0");
  EXPECT_EQ(Lengthd(50.0, Lengthd::Unit::Percent).toRcString(), "50%");
  EXPECT_EQ(Lengthd(-25.0, Lengthd::Unit::Px).toRcString(), "-25px");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Em).toRcString(), "1em");
}

TEST(LengthTest, ToRcStringFractionalValues) {
  EXPECT_EQ(Lengthd(1.5, Lengthd::Unit::Em).toRcString(), "1.5em");
  EXPECT_EQ(Lengthd(0.25, Lengthd::Unit::Px).toRcString(), "0.25px");
  // `{:g}` trims trailing zeros and prefers the shortest round-trippable form.
  EXPECT_EQ(Lengthd(3.14, Lengthd::Unit::Px).toRcString(), "3.14px");
  EXPECT_EQ(Lengthd(-0.5, Lengthd::Unit::Percent).toRcString(), "-0.5%");
}

TEST(LengthTest, ToRcStringCoversAllUnits) {
  // Integer value (1) with every unit identifier, to lock in suffix mapping.
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::None).toRcString(), "1");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Percent).toRcString(), "1%");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Cm).toRcString(), "1cm");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Mm).toRcString(), "1mm");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Q).toRcString(), "1q");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::In).toRcString(), "1in");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Pc).toRcString(), "1pc");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Pt).toRcString(), "1pt");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Px).toRcString(), "1px");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Em).toRcString(), "1em");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Ex).toRcString(), "1ex");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Ch).toRcString(), "1ch");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Rem).toRcString(), "1rem");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Vw).toRcString(), "1vw");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Vh).toRcString(), "1vh");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Vmin).toRcString(), "1vmin");
  EXPECT_EQ(Lengthd(1.0, Lengthd::Unit::Vmax).toRcString(), "1vmax");
}

}  // namespace donner
