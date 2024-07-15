#include "donner/svg/SVGPatternElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/XMLTestUtils.h"

namespace donner::svg {

TEST(SVGPatternElementTests, Defaults) {
  auto pattern = instantiateSubtreeElementAs<SVGPatternElement>("<pattern />");

  EXPECT_THAT(pattern->viewbox(), testing::Eq(std::nullopt));
  EXPECT_THAT(pattern->preserveAspectRatio(),
              testing::Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                              PreserveAspectRatio::MeetOrSlice::Meet}));

  EXPECT_THAT(pattern->x(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(pattern->y(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(pattern->width(), testing::Eq(std::nullopt));
  EXPECT_THAT(pattern->height(), testing::Eq(std::nullopt));

  EXPECT_THAT(pattern->patternUnits(), testing::Eq(PatternUnits::ObjectBoundingBox));
  EXPECT_THAT(pattern->patternContentUnits(), testing::Eq(PatternContentUnits::UserSpaceOnUse));
  EXPECT_THAT(pattern->patternTransform(), TransformEq(Transformd()));
  EXPECT_THAT(pattern->href(), testing::Eq(std::nullopt));
}

TEST(SVGPatternElementTests, PatternUnits) {
  auto pattern = instantiateSubtreeElementAs<SVGPatternElement>(R"(
    <pattern patternUnits="userSpaceOnUse" />
  )");

  EXPECT_THAT(pattern->patternUnits(), testing::Eq(PatternUnits::UserSpaceOnUse));

  pattern->setPatternUnits(PatternUnits::ObjectBoundingBox);
  EXPECT_THAT(pattern->patternUnits(), testing::Eq(PatternUnits::ObjectBoundingBox));
}

TEST(SVGPatternElementTests, PatternContentUnits) {
  auto pattern = instantiateSubtreeElementAs<SVGPatternElement>(R"(
    <pattern patternContentUnits="objectBoundingBox" />
  )");

  EXPECT_THAT(pattern->patternContentUnits(), testing::Eq(PatternContentUnits::ObjectBoundingBox));

  pattern->setPatternContentUnits(PatternContentUnits::UserSpaceOnUse);
  EXPECT_THAT(pattern->patternContentUnits(), testing::Eq(PatternContentUnits::UserSpaceOnUse));
}

TEST(SVGPatternElementTests, ObjectBoundingBoxRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" width="1" height="1">
          <circle r="4" cx="4" cy="4" fill="lime" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ..####..........
        .######.........
        ########........
        ########........
        ########........
        ########........
        .######.........
        ..####..........
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        )"));
}

TEST(SVGPatternElementTests, ObjectBoundingBoxTiledRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" width="0.5" height="0.5">
          <rect x="0" y="0" width="4" height="4" fill="lime" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ####....####....
        ####....####....
        ####....####....
        ####....####....
        ................
        ................
        ................
        ................
        ####....####....
        ####....####....
        ####....####....
        ####....####....
        ................
        ................
        ................
        ................
        )"));
}

TEST(SVGPatternElementTests, ObjectBoundingBoxTiledWithXYRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" x="0.125" y="0.25" width="0.5" height="0.5">
          <rect x="0" y="0" width="4" height="4" fill="lime" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ..####....####..
        ..####....####..
        ..####....####..
        ..####....####..
        ................
        ................
        ................
        ................
        ..####....####..
        ..####....####..
        ..####....####..
        ..####....####..
        )"));
}

TEST(SVGPatternElementTests, UserSpaceOnUseRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect x="0" y="0" width="4" height="4" fill="lime" />
          <rect x="4" y="4" width="4" height="4" fill="gray" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ####....####....
        ####....####....
        ####....####....
        ####....####....
        ....++++....++++
        ....++++....++++
        ....++++....++++
        ....++++....++++
        ####....####....
        ####....####....
        ####....####....
        ####....####....
        ....++++....++++
        ....++++....++++
        ....++++....++++
        ....++++....++++
        )"));
}

TEST(SVGPatternElementTests, UserSpaceOnUseWithXYRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" x="2" y="2" width="6" height="6">
          <rect x="0" y="0" width="4" height="4" fill="lime" />
          <rect x="4" y="4" width="4" height="4" fill="gray" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ++....++....++..
        ++....++....++..
        ..####..####..##
        ..####..####..##
        ..####..####..##
        ..####..####..##
        ++....++....++..
        ++....++....++..
        ..####..####..##
        ..####..####..##
        ..####..####..##
        ..####..####..##
        ++....++....++..
        ++....++....++..
        ..####..####..##
        ..####..####..##
        )"));
}

TEST(SVGPatternElementTests, PatternContentObjectBoundingBoxRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternContentUnits="objectBoundingBox" width="0.5" height="0.5">
          <rect x="0" y="0" width="0.25" height="0.25" fill="lime" />
          <rect x="0.125" y="0.125" width="0.25" height="0.25" fill="gray" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ####....####....
        ####....####....
        ##++++..##++++..
        ##++++..##++++..
        ..++++....++++..
        ..++++....++++..
        ................
        ................
        ####....####....
        ####....####....
        ##++++..##++++..
        ##++++..##++++..
        ..++++....++++..
        ..++++....++++..
        ................
        ................
        )"));
}

/**
 * Tests the interaction between non-default values for patternUnits, patternContentUnits, and a
 * tile rect with x/y.
 */
TEST(SVGPatternElementTests, UnitsNonDefaultWithXYRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" patternContentUnits="objectBoundingBox" x="4" y="4" width="4" height="4">
          <rect x="0" y="0" width="0.25" height="0.25" fill="lime" />
          <rect x="0.125" y="0.125" width="0.25" height="0.25" fill="gray" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ################
        ################
        ##++##++##++##++
        ##++##++##++##++
        ################
        ################
        ##++##++##++##++
        ##++##++##++##++
        ################
        ################
        ##++##++##++##++
        ##++##++##++##++
        ################
        ################
        ##++##++##++##++
        ##++##++##++##++
        )"));
}

TEST(SVGPatternElementTests, PatternTransform) {
  auto pattern = instantiateSubtreeElementAs<SVGPatternElement>(R"-(
    <pattern patternTransform="scale(2)" />
  )-");

  EXPECT_THAT(pattern->patternTransform(), TransformIs(2, 0, 0, 2, 0, 0));

  pattern->setPatternTransform(Transformd::Translate(Vector2d(3.0, 5.0)));
  EXPECT_THAT(pattern->patternTransform(), TransformIs(1, 0, 0, 1, 3.0, 5.0));
}

TEST(SVGPatternElementTests, PatternTransformRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" width="0.5" height="0.5" patternTransform="skewX(45) scale(2)">
          <rect x="0" y="0" width="4" height="4" fill="lime" />
          <rect x="4" y="4" width="4" height="4" fill="gray" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        -#######-.......
        .-#######-......
        ..-#######-.....
        ...-#######-....
        ....-#######-...
        .....-#######-..
        ......-#######-.
        .......-#######-
        :-------::::::::
        .:+++++++:......
        ..:+++++++:.....
        ...:+++++++:....
        ....:+++++++:...
        .....:+++++++:..
        ......:+++++++:.
        .......:+++++++:
        )"));
}

TEST(SVGPatternElementTests, PatternTransformWithXYRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" x="0.125" y="0.25" width="0.5" height="0.5" patternTransform="rotate(45) scale(2 1)">
          <rect x="0" y="0" width="4" height="4" fill="lime" />
          <rect x="4" y="4" width="4" height="4" fill="gray" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        :....+,....:####
        +:..+##,....:##-
        ++:+####,....:-.
        +=,-#####,...:-.
        =,..-#####,.:++-
        ,....-#####:++++
        +.....-###-.-+++
        #+.....-#-...-++
        ##+.....-.....-+
        ###+...:+:.....-
        ####+.:+++:.....
        +####-=++++:....
        .+##-.,=++++:..:
        ..+-...,=++++::#
        ..:,....,=+++--#
        .:++,....,=+-..-
        )"));
}
TEST(SVGPatternElementTests, PatternTransformWithPatternUnitsRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8" patternTransform="skewX(45) scale(2)">
          <rect x="0" y="0" width="4" height="4" fill="lime" />
          <rect x="4" y="4" width="4" height="4" fill="gray" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        -#######-.......
        .-#######-......
        ..-#######-.....
        ...-#######-....
        ....-#######-...
        .....-#######-..
        ......-#######-.
        .......-#######-
        :-------::::::::
        .:+++++++:......
        ..:+++++++:.....
        ...:+++++++:....
        ....:+++++++:...
        .....:+++++++:..
        ......:+++++++:.
        .......:+++++++:
        )"));
}

TEST(SVGPatternElementTests, PatternTransformWithPatternUnitsAndXYRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" x="2" y="2" width="8" height="8" patternTransform="skewX(45) scale(2)">
          <rect x="0" y="0" width="4" height="4" fill="lime" />
          <rect x="4" y="4" width="4" height="4" fill="gray" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ++++:.......:+++
        +++++:.......:++
        ++++++:.......:+
        +++++++:.......:
        -.......-#######
        #-.......-######
        ##-.......-#####
        ###-.......-####
        ####-.......-###
        #####-.......-##
        ######-.......-#
        #######-.......-
        :::::::::-------
        +:.......:++++++
        ++:.......:+++++
        +++:.......:++++
        )"));
}

// TODO viewBox/preserveAspectRatio
// TODO: Additional tests and rendering tests

}  // namespace donner::svg
