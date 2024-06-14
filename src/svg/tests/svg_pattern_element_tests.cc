#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/tests/base_test_utils.h"
#include "src/svg/core/preserve_aspect_ratio.h"
#include "src/svg/renderer/tests/renderer_test_utils.h"
#include "src/svg/svg_pattern_element.h"
#include "src/svg/tests/xml_test_utils.h"

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

// TODO: Additional tests and rendering tests

}  // namespace donner::svg
