#include "donner/base/Length.h"

#include <gtest/gtest.h>

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
