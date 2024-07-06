#include "donner/base/Length.h"

#include <gtest/gtest.h>

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

}  // namespace donner
