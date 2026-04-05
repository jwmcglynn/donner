#include "donner/svg/SVGClipPathElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;

namespace donner::svg {

TEST(SVGClipPathElementTests, Defaults) {
  auto clipPath = instantiateSubtreeElementAs<SVGClipPathElement>("<clipPath />");
  EXPECT_EQ(clipPath->clipPathUnits(), ClipPathUnits::UserSpaceOnUse);
}

TEST(SVGClipPathElementTests, SetClipPathUnits) {
  auto clipPath = instantiateSubtreeElementAs<SVGClipPathElement>(
      "<clipPath clipPathUnits=\"objectBoundingBox\" />");
  EXPECT_EQ(clipPath->clipPathUnits(), ClipPathUnits::ObjectBoundingBox);

  clipPath->setClipPathUnits(ClipPathUnits::UserSpaceOnUse);
  EXPECT_EQ(clipPath->clipPathUnits(), ClipPathUnits::UserSpaceOnUse);
}

/**
 * If an invalid value is provided for clipPathUnits, the parser should fall back to the default.
 */
TEST(SVGClipPathElementTests, InvalidClipPathUnits) {
  auto clipPath =
      instantiateSubtreeElementAs<SVGClipPathElement>("<clipPath clipPathUnits=\"invalid\" />");

  // Assuming that an invalid value falls back to UserSpaceOnUse.
  EXPECT_EQ(clipPath->clipPathUnits(), ClipPathUnits::UserSpaceOnUse);
}

TEST(SVGClipPathElementTests, RenderingDefaults) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="a">
          <circle cx="8" cy="8" r="8" />
        </clipPath>
        <rect width="16" height="16" clip-path="url(#a)" fill="white" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ....,=#@@#=,....
        ...#@@@@@@@@*...
        .,%@@@@@@@@@@%,.
        .*@@@@@@@@@@@@*.
        ,@@@@@@@@@@@@@@,
        +@@@@@@@@@@@@@@+
        %@@@@@@@@@@@@@@%
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        %@@@@@@@@@@@@@@%
        +@@@@@@@@@@@@@@+
        ,@@@@@@@@@@@@@@,
        .*@@@@@@@@@@@@*.
        ..%@@@@@@@@@@%..
        ...#@@@@@@@@#...
        ....,=#@@#=,....
        )"));
}

TEST(SVGClipPathElementTests, RenderingObjectBoundingBox) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="a" clipPathUnits="objectBoundingBox">
          <circle cx="0.5" cy="0.5" r="0.5" />
        </clipPath>
        <rect width="8" height="8" clip-path="url(#a)" fill="white" />
        <rect y="8" width="16" height="8" clip-path="url(#a)" fill="white" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ..#@@#,.........
        ,@@@@@@.........
        *@@@@@@*........
        @@@@@@@@........
        @@@@@@@@........
        *@@@@@@*........
        ,@@@@@@.........
        .,#@@#..........
        ...:*%@@@@%*:...
        .:%@@@@@@@@@@%:.
        -@@@@@@@@@@@@@@-
        %@@@@@@@@@@@@@@%
        @@@@@@@@@@@@@@@%
        -@@@@@@@@@@@@@@-
        .:%@@@@@@@@@@%:.
        ...:*%@@@@%*:...
        )"));
}

/**
 * If a clipPath element is empty, then nothing should be rendered (i.e. the clipping region is
 * empty).
 */
TEST(SVGClipPathElementTests, RenderingEmptyClipPath) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="emptyClip" />
        <rect width="16" height="16" clip-path="url(#emptyClip)" fill="white" />
        )-");
  // Expect that the rendered rectangle is fully clipped, resulting in an empty (blank) image.
  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
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

/**
 * Verify that clip-rule "nonzero" is correctly applied when rendering.
 */
TEST(SVGClipPathElementTests, ClipRuleNonzero) {
  const AsciiImage nonzeroResult = RendererTestUtils::renderToAsciiImage(R"-(
        <defs>
          <clipPath id="clip-nonzero" clipPathUnits="userSpaceOnUse" clip-rule="nonzero">
            <path d="M4,4 h8 v8 h-8 Z M6,6 h4 v4 h-4 Z" />
          </clipPath>
        </defs>
        <rect x="0" y="0" width="16" height="16" fill="black"/>
        <rect x="0" y="0" width="16" height="16" fill="white" clip-path="url(#clip-nonzero)"/>
        )-");

  EXPECT_TRUE(nonzeroResult.matches(R"(
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

/**
 * Verify that clip-rule "evenodd" is correctly applied when rendering.
 */
TEST(SVGClipPathElementTests, ClipRuleEvenodd) {
  const AsciiImage evenoddResult = RendererTestUtils::renderToAsciiImage(R"-(
        <defs>
          <clipPath id="clip-evenodd" clipPathUnits="userSpaceOnUse" clip-rule="evenodd">
            <path d="M4,4 h8 v8 h-8 Z M6,6 h4 v4 h-4 Z" />
          </clipPath>
        </defs>
        <rect x="0" y="0" width="16" height="16" fill="black"/>
        <rect x="0" y="0" width="16" height="16" fill="white" clip-path="url(#clip-evenodd)"/>
        )-");

  EXPECT_TRUE(evenoddResult.matches(R"(
  ................
  ................
  ................
  ................
  ....@@@@@@@@....
  ....@@@@@@@@....
  ....@@....@@....
  ....@@....@@....
  ....@@....@@....
  ....@@....@@....
  ....@@@@@@@@....
  ....@@@@@@@@....
  ................
  ................
  ................
  ................
  )"));
}

/**
 * Verify that a clipPath element containing multiple paths with different clip rules
 * (specified on each child) is applied correctly when rendering.
 */
TEST(SVGClipPathElementTests, MultiplePathsWithDifferentClipRulesSideBySide) {
  const AsciiImage result = RendererTestUtils::renderToAsciiImage(R"-(
        <defs>
          <clipPath id="multi-clip" clipPathUnits="userSpaceOnUse">
            <path d="M1,2 h6 v12 h-6 Z M2,4 h4 v8 h-4 Z" clip-rule="evenodd" />
            <path d="M9,2 h6 v12 h-6 Z M10,4 h4 v8 h-4 Z" clip-rule="nonzero" />
          </clipPath>
        </defs>
        <rect x="0" y="0" width="16" height="16" fill="black"/>
        <rect x="0" y="0" width="16" height="16" fill="white" clip-path="url(#multi-clip)"/>
        )-");

  EXPECT_TRUE(result.matches(R"(
  ................
  ................
  .@@@@@@..@@@@@@.
  .@@@@@@..@@@@@@.
  .@....@..@@@@@@.
  .@....@..@@@@@@.
  .@....@..@@@@@@.
  .@....@..@@@@@@.
  .@....@..@@@@@@.
  .@....@..@@@@@@.
  .@....@..@@@@@@.
  .@....@..@@@@@@.
  .@@@@@@..@@@@@@.
  .@@@@@@..@@@@@@.
  ................
  ................
  )"));
}

/**
 * Verify that transforms on elements within a clipPath are applied correctly.
 */
TEST(SVGClipPathElementTests, RenderingTransform) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="clipTransform">
          <circle cx="8" cy="8" r="8" transform="translate(2 2)" />
        </clipPath>
        <rect width="16" height="16" clip-path="url(#clipTransform)" fill="white" />
        )-");

  // Expected output assumes that the translated circle defines a shifted clipping region.
  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ......,=#@@#+,..
        .....*@@@@@@@@*.
        ...,%@@@@@@@@@@*
        ...*@@@@@@@@@@@@
        ..,@@@@@@@@@@@@@
        ..+@@@@@@@@@@@@@
        ..%@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..%@@@@@@@@@@@@@
        ..+@@@@@@@@@@@@@
        ..,@@@@@@@@@@@@@
        ...+@@@@@@@@@@@@
        ....#@@@@@@@@@@*
        )"));
}

/**
 * Verify that a clipPath with multiple child elements—each potentially having their own transforms—
 * is correctly applied when rendering.
 */
TEST(SVGClipPathElementTests, RenderingMultipleChildrenWithTransforms) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="clipMultiTrans">
          <circle cx="4" cy="4" r="4" transform="translate(4,0)" />
          <rect x="0" y="4" width="8" height="4" />
        </clipPath>
        <rect width="16" height="16" clip-path="url(#clipMultiTrans)" fill="white" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
      ......#@@#......
      .....@@@@@@.....
      ....*@@@@@@*....
      ....@@@@@@@@....
      @@@@@@@@@@@@....
      @@@@@@@@@@@*....
      @@@@@@@@@@@.....
      @@@@@@@@@#......
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

}  // namespace donner::svg
