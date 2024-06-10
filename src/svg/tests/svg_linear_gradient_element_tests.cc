#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/tests/base_test_utils.h"
#include "src/svg/core/gradient.h"
#include "src/svg/renderer/tests/renderer_test_utils.h"
#include "src/svg/svg_linear_gradient_element.h"
#include "src/svg/tests/xml_test_utils.h"

using testing::AllOf;

namespace donner::svg {

MATCHER_P2(LengthIs, valueMatcher, unitMatcher, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.unit, result_listener);
}

TEST(SVGLinearGradientElementTests, Defaults) {
  auto gradient = instantiateSubtreeElementAs<SVGLinearGradientElement>("<linearGradient />");
  EXPECT_THAT(gradient->x1(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->y1(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->x2(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->y2(), testing::Eq(std::nullopt));

  EXPECT_THAT(gradient->href(), testing::Eq(std::nullopt));
  EXPECT_THAT(gradient->gradientUnits(), testing::Eq(GradientUnits::ObjectBoundingBox));
  EXPECT_THAT(gradient->gradientTransform(), TransformEq(Transformd()));
  EXPECT_THAT(gradient->spreadMethod(), testing::Eq(GradientSpreadMethod::Pad));
}

TEST(SVGLinearGradientElementTests, RenderingDefaults) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <linearGradient id="a">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </linearGradient>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        @@%##**+==-::,..
        )"));
}

TEST(SVGLinearGradientElementTests, RenderingTransform) {
  ParsedFragment<SVGLinearGradientElement> fragment =
      instantiateSubtreeElementAs<SVGLinearGradientElement>(R"-(
        <linearGradient id="a" gradientTransform="rotate(45)">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </linearGradient>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  constexpr double kInvSqrt2 = MathConstants<double>::kInvSqrt2;

  EXPECT_THAT(fragment->gradientTransform(),
              TransformIs(kInvSqrt2, kInvSqrt2, -kInvSqrt2, kInvSqrt2, 0, 0));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        @@%%###**++==---
        @%%###**++==---:
        %%###**++==---::
        %###**++==---::,
        ###**++==---::,,
        ##**++==---::,,.
        #**++==---::,,..
        **++==---::,,...
        *++==---::,,....
        ++==---::,,.....
        +==---::,,......
        ==---::,,.......
        =---::,,........
        ---::,,.........
        --::,,..........
        -::,,...........
        )"));
  }

  fragment->setGradientTransform(Transformd::Rotation(90.0 * MathConstants<double>::kDegToRad));

  EXPECT_THAT(fragment->gradientTransform(), TransformIs(0, 1, -1, 0, 0, 0));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        %%%%%%%%%%%%%%%%
        ################
        ################
        ****************
        ****************
        ++++++++++++++++
        ================
        ================
        ----------------
        ::::::::::::::::
        ::::::::::::::::
        ,,,,,,,,,,,,,,,,
        ................
        ................
        )"));
  }
}

}  // namespace donner::svg
