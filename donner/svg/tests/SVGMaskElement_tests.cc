#include "donner/svg/SVGMaskElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;

namespace donner::svg {

TEST(SVGMaskElementTests, Defaults) {
  auto mask = instantiateSubtreeElementAs<SVGMaskElement>("<mask />");
  EXPECT_EQ(mask->maskUnits(), ClipPathUnits::UserSpaceOnUse);
  EXPECT_EQ(mask->maskContentUnits(), ClipPathUnits::UserSpaceOnUse);
}

TEST(SVGMaskElementTests, SetMaskUnits) {
  auto mask =
      instantiateSubtreeElementAs<SVGMaskElement>("<mask maskUnits=\"objectBoundingBox\" />");
  EXPECT_EQ(mask->maskUnits(), ClipPathUnits::ObjectBoundingBox);
}

TEST(SVGMaskElementTests, SetMaskContentUnits) {
  auto mask = instantiateSubtreeElementAs<SVGMaskElement>(
      "<mask maskContentUnits=\"objectBoundingBox\" />");
  EXPECT_EQ(mask->maskContentUnits(), ClipPathUnits::ObjectBoundingBox);
}

TEST(SVGMaskElementTests, RenderingDefaults) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <mask id="a">
          <circle cx="8" cy="8" r="8" fill="white" />
        </mask>
        <rect width="16" height="16" mask="url(#a)" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ................
        ................
        ................
        ................
        )"));
}

TEST(SVGMaskElementTests, RenderingObjectBoundingBox) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <mask id="a" maskUnits="objectBoundingBox">
          <circle cx="0.5" cy="0.5" r="0.5" fill="white" />
        </mask>
        <rect width="8" height="8" mask="url(#a)" fill="black" />
        <rect y="8" width="16" height="8" mask="url(#a)" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ................
        ................
        ................
        ................
        )"));
}

TEST(SVGMaskElementTests, RenderingMaskContentUnits) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <mask id="a" maskContentUnits="objectBoundingBox">
          <circle cx="0.5" cy="0.5" r="0.5" fill="white" />
        </mask>
        <rect width="16" height="16" mask="url(#a)" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ................
        ................
        ................
        ................
        )"));
}

TEST(SVGMaskElementTests, RenderingMaskUnitsAndContentUnits) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <mask id="a" maskUnits="objectBoundingBox" maskContentUnits="objectBoundingBox">
          <circle cx="0.5" cy="0.5" r="0.5" fill="white" />
        </mask>
        <rect width="8" height="8" mask="url(#a)" fill="black" />
        <rect y="8" width="16" height="8" mask="url(#a)" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ....@@@@@@@@....
        ................
        ................
        ................
        ................
        )"));
}

}  // namespace donner::svg
