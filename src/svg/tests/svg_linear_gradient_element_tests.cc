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

TEST(SVGLinearGradientElementTests, GradientCoordinates) {
  ParsedFragment<SVGLinearGradientElement> fragment =
      instantiateSubtreeElementAs<SVGLinearGradientElement>(R"-(
        <linearGradient id="a" x1="12.5%" y1="25%" x2="75%" y2="87.5%">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </linearGradient>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_THAT(fragment->x1(), testing::Optional(LengthIs(12.5, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->y1(), testing::Optional(LengthIs(25, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->x2(), testing::Optional(LengthIs(75, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->y2(), testing::Optional(LengthIs(87.5, Lengthd::Unit::Percent)));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@@%%##**++
        @@@@@@@%%##**++=
        @@@@@@%%##**++==
        @@@@@%%##**++==-
        @@@@%%##**++==--
        @@@%%##**++==--:
        @@%%##**++==--::
        @%%##**++==--::,
        %%##**++==--::,,
        %##**++==--::,,.
        ##**++==--::,,..
        #**++==--::,,...
        **++==--::,,....
        *++==--::,,.....
        ++==--::,,......
        +==--::,,.......
        )"));
  }

  fragment->setX1(Lengthd(0, Lengthd::Unit::Percent));
  fragment->setX2(Lengthd(0, Lengthd::Unit::Percent));

  // Verify that the properties are updated.
  EXPECT_THAT(fragment->x1(), testing::Optional(LengthIs(0, Lengthd::Unit::Percent)));
  EXPECT_THAT(fragment->y1(), testing::Optional(LengthIs(25, Lengthd::Unit::Percent)));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
    EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        %%%%%%%%%%%%%%%%
        ################
        ****************
        ++++++++++++++++
        ================
        ----------------
        ::::::::::::::::
        ,,,,,,,,,,,,,,,,
        ................
        ................
        ................
        )"));
  }
}

TEST(SVGLinearGradientElementTests, GradientUnitsUserSpaceOnUse) {
  auto gradient = instantiateSubtreeElementAs<SVGLinearGradientElement>(
      R"(<linearGradient gradientUnits="userSpaceOnUse" />")");
  EXPECT_THAT(gradient->gradientUnits(), testing::Eq(GradientUnits::UserSpaceOnUse));
}

TEST(SVGLinearGradientElementTests, GradientUnitsObjectBoundingBox) {
  auto gradient = instantiateSubtreeElementAs<SVGLinearGradientElement>(
      R"(<linearGradient gradientUnits="objectBoundingBox" />")");
  EXPECT_THAT(gradient->gradientUnits(), testing::Eq(GradientUnits::ObjectBoundingBox));
}
TEST(SVGLinearGradientElementTests, GradientUnitsRendering) {
  ParsedFragment<SVGLinearGradientElement> fragment =
      instantiateSubtreeElementAs<SVGLinearGradientElement>(R"-(
        <linearGradient id="a" gradientUnits="userSpaceOnUse" x1="2" y1="2" x2="14" y2="14">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </linearGradient>
        <rect x="0" y="0" width="8" height="8" fill="url(#a)" />
        <rect x="8" y="8" width="8" height="8" fill="url(#a)" />
        )-");

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@%%........
        @@@@@%%%........
        @@@@%%%#........
        @@@%%%##........
        @@%%%##*........
        @%%%##**........
        %%%##**+........
        %%##**++........
        ........==--:::,
        ........=--:::,,
        ........--:::,,.
        ........-:::,,..
        ........:::,,...
        ........::,,....
        ........:,,.....
        ........,,......
        )"));
  }

  // Change gradientUnits, rendering should change.
  fragment->setGradientUnits(GradientUnits::ObjectBoundingBox);

  EXPECT_EQ(fragment->gradientUnits(), GradientUnits::ObjectBoundingBox);

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
    EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        ........@@@@@@@@
        ........@@@@@@@@
        ........@@@@@@@@
        ........@@@@@@@@
        ........@@@@@@@@
        ........@@@@@@@@
        ........@@@@@@@@
        ........@@@@@@@@
        )"));
  }
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

TEST(SVGLinearGradientElementTests, SpreadMethodPad) {
  auto gradient = instantiateSubtreeElementAs<SVGLinearGradientElement>(
      R"(<linearGradient spreadMethod="pad" />)");
  EXPECT_THAT(gradient->spreadMethod(), testing::Eq(GradientSpreadMethod::Pad));
}

TEST(SVGLinearGradientElementTests, SpreadMethodReflect) {
  auto gradient = instantiateSubtreeElementAs<SVGLinearGradientElement>(
      R"(<linearGradient spreadMethod="reflect" />)");
  EXPECT_THAT(gradient->spreadMethod(), testing::Eq(GradientSpreadMethod::Reflect));
}

TEST(SVGLinearGradientElementTests, SpreadMethodRepeat) {
  auto gradient = instantiateSubtreeElementAs<SVGLinearGradientElement>(
      R"(<linearGradient spreadMethod="repeat" />)");
  EXPECT_THAT(gradient->spreadMethod(), testing::Eq(GradientSpreadMethod::Repeat));
}

TEST(SVGLinearGradientElementTests, SpreadMethodRendering) {
  ParsedFragment<SVGLinearGradientElement> fragment =
      instantiateSubtreeElementAs<SVGLinearGradientElement>(R"-(
        <linearGradient id="a" spreadMethod="pad" x1="12.5%" y1="25%" x2="75%" y2="87.5%">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </linearGradient>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@@%%##**++
        @@@@@@@%%##**++=
        @@@@@@%%##**++==
        @@@@@%%##**++==-
        @@@@%%##**++==--
        @@@%%##**++==--:
        @@%%##**++==--::
        @%%##**++==--::,
        %%##**++==--::,,
        %##**++==--::,,.
        ##**++==--::,,..
        #**++==--::,,...
        **++==--::,,....
        *++==--::,,.....
        ++==--::,,......
        +==--::,,.......
        )"));
  }

  // Change spreadMethod to reflect, rendering should change.
  fragment->setSpreadMethod(GradientSpreadMethod::Reflect);

  EXPECT_EQ(fragment->spreadMethod(), GradientSpreadMethod::Reflect);

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
    EXPECT_TRUE(generatedAscii.matches(R"(
        #%%@@@@@%%##**++
        %%@@@@@%%##**++=
        %@@@@@%%##**++==
        @@@@@%%##**++==-
        @@@@%%##**++==--
        @@@%%##**++==--:
        @@%%##**++==--::
        @%%##**++==--::,
        %%##**++==--::,,
        %##**++==--::,,.
        ##**++==--::,,..
        #**++==--::,,...
        **++==--::,,...,
        *++==--::,,...,,
        ++==--::,,...,,:
        +==--::,,...,,::
        )"));
  }

  // Change spreadMethod to repeat, rendering should change.
  fragment->setSpreadMethod(GradientSpreadMethod::Repeat);

  EXPECT_EQ(fragment->spreadMethod(), GradientSpreadMethod::Repeat);

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
    EXPECT_TRUE(generatedAscii.matches(R"(
        ::,,..@@%%##**++
        :,,..@@%%##**++=
        ,,..@@%%##**++==
        ,..@@%%##**++==-
        ..@@%%##**++==--
        .@@%%##**++==--:
        @@%%##**++==--::
        @%%##**++==--::,
        %%##**++==--::,,
        %##**++==--::,,.
        ##**++==--::,,.@
        #**++==--::,,.@@
        **++==--::,,.@@@
        *++==--::,,.@@@%
        ++==--::,,.@@@%%
        +==--::,,.@@@%%#
        )"));
  }
}

TEST(SVGLinearGradientElementTests, HrefSimple) {
  auto gradient = instantiateSubtreeElementAs<SVGLinearGradientElement>(
      R"(<linearGradient href="#refGradient" />)");
  EXPECT_THAT(gradient->href(), testing::Optional(testing::Eq("#refGradient")));
}

TEST(SVGLinearGradientElementTests, HrefInheritanceChildrenXYRendering) {
  ParsedFragment<SVGLinearGradientElement> fragment =
      instantiateSubtreeElementAs<SVGLinearGradientElement>(R"-(
        <linearGradient id="gradient" href="#refGradient" />
        <linearGradient id="refGradient" x1="10%" y1="20%" x2="80%" y2="90%">
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </linearGradient>
        <rect width="16" height="16" fill="url(#gradient)" />
        )-");

  EXPECT_THAT(fragment->href(), testing::Optional(testing::Eq("#refGradient")));
  EXPECT_THAT(fragment->x1(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->y1(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->x2(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->y2(), testing::Eq(std::nullopt));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@%%##***++
        @@@@@@%%##***++=
        @@@@@%%##***++==
        @@@@%%##***++==-
        @@@%%##***++==--
        @@%%##***++==--:
        @%%##***++==--::
        %%##***++==--::,
        %##***++==--::,,
        ##***++==--::,,,
        #***++==--::,,,.
        ***++==--::,,,..
        **++==--::,,,...
        *++==--::,,,....
        ++==--::,,,.....
        +==--::,,,......
        )"));
  }
}

TEST(SVGLinearGradientElementTests, HrefInheritanceSharedParamsRendering) {
  ParsedFragment<SVGLinearGradientElement> fragment =
      instantiateSubtreeElementAs<SVGLinearGradientElement>(R"-(
        <linearGradient id="gradient" href="#refGradient" gradientUnits="userSpaceOnUse"
            gradientTransform="rotate(90)" spreadMethod="repeat">
          <!-- should be overridden -->
          <stop offset="0%" stop-color="white" />
          <stop offset="100%" stop-color="black" />
        </linearGradient>
        <linearGradient id="refGradient" x1="10%" x2="80%">
          <stop offset="20%" stop-color="white" />
          <stop offset="80%" stop-color="black" />
        </linearGradient>
        <rect width="16" height="16" fill="url(#gradient)" />
        )-");

  EXPECT_THAT(fragment->href(), testing::Optional(testing::Eq("#refGradient")));
  EXPECT_THAT(fragment->x1(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->y1(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->x2(), testing::Eq(std::nullopt));
  EXPECT_THAT(fragment->y2(), testing::Eq(std::nullopt));

  {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);

    EXPECT_TRUE(generatedAscii.matches(R"(
        ,,,,,,,,,,,,,,,,
        ................
        @@@@@@@@@@@@@@@@
        %%%%%%%%%%%%%%%%
        ################
        ****************
        ++++++++++++++++
        ================
        ----------------
        ----------------
        ::::::::::::::::
        ,,,,,,,,,,,,,,,,
        ................
        @@@@@@@@@@@@@@@@
        %%%%%%%%%%%%%%%%
        ################
        )"));
  }
}

}  // namespace donner::svg
