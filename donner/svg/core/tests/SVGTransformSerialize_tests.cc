#include "donner/svg/core/SVGTransformSerialize.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/MathUtils.h"

namespace donner::svg {
namespace {

TEST(SVGTransformSerializeTest, IdentityReturnsEmptyString) {
  EXPECT_EQ(std::string_view(toSVGTransformString(Transformd())), "");
}

TEST(SVGTransformSerializeTest, PureTranslation) {
  EXPECT_EQ(std::string_view(toSVGTransformString(Transformd::Translate(10.0, 20.0))),
            "translate(10, 20)");
}

TEST(SVGTransformSerializeTest, PureTranslationFractional) {
  EXPECT_EQ(std::string_view(toSVGTransformString(Transformd::Translate(1.5, -3.25))),
            "translate(1.5, -3.25)");
}

TEST(SVGTransformSerializeTest, PureUniformScale) {
  EXPECT_EQ(std::string_view(toSVGTransformString(Transformd::Scale(2.0))), "scale(2)");
}

TEST(SVGTransformSerializeTest, PureNonUniformScale) {
  EXPECT_EQ(std::string_view(toSVGTransformString(Transformd::Scale(Vector2d(2.0, 3.0)))),
            "scale(2, 3)");
}

TEST(SVGTransformSerializeTest, RotationUsesMatrixFormat) {
  const Transformd rotation = Transformd::Rotate(MathConstants<double>::kHalfPi);
  const std::string result(toSVGTransformString(rotation));
  // Rotation can't be simplified to translate/scale, so it should use matrix().
  EXPECT_THAT(result, testing::StartsWith("matrix("));
  EXPECT_THAT(result, testing::EndsWith(")"));
}

TEST(SVGTransformSerializeTest, TranslateAndScaleUsesMatrix) {
  const Transformd combined = Transformd::Translate(5.0, 10.0) * Transformd::Scale(2.0);
  const std::string result(toSVGTransformString(combined));
  // Combined transform uses matrix format.
  EXPECT_THAT(result, testing::StartsWith("matrix("));
}

}  // namespace
}  // namespace donner::svg
