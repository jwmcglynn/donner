#include "donner/svg/SVGClipPathElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>

#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
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

TEST(SVGClipPathElementTests, SimpleResolvedOutlineUsesDocumentTransformAndUnits) {
  SVGDocument document = instantiateSubtree(R"svg(
    <defs>
      <clipPath id="user"><rect x="2" y="4" width="20" height="24"/></clipPath>
      <clipPath id="box" clipPathUnits="objectBoundingBox">
        <rect x="0.25" y="0.25" width="0.5" height="0.5"/>
      </clipPath>
    </defs>
    <g transform="translate(10 5)">
      <rect id="user-target" x="20" y="30" width="40" height="50" clip-path="url(#user)"/>
      <rect id="box-target" x="20" y="30" width="40" height="50" clip-path="url(#box)"/>
    </g>
  )svg",
                                            {}, Vector2i(120, 120));
  Renderer renderer;
  renderer.draw(document);

  const auto userTarget = document.querySelector("#user-target");
  const auto boxTarget = document.querySelector("#box-target");
  ASSERT_TRUE(userTarget.has_value());
  ASSERT_TRUE(boxTarget.has_value());
  const auto userOutline = userTarget->cast<SVGGraphicsElement>().resolvedSimpleClipPathOutline(
      /*maxVerbs=*/128, /*maxPoints=*/256, /*maxBytes=*/16u * 1024u);
  const auto boxOutline = boxTarget->cast<SVGGraphicsElement>().resolvedSimpleClipPathOutline(
      /*maxVerbs=*/128, /*maxPoints=*/256, /*maxBytes=*/16u * 1024u);
  ASSERT_TRUE(userOutline.has_value());
  ASSERT_TRUE(boxOutline.has_value());
  EXPECT_EQ(userOutline->bounds(), Box2d::FromXYWH(12.0, 9.0, 20.0, 24.0));
  EXPECT_EQ(boxOutline->bounds(), Box2d::FromXYWH(40.0, 47.5, 20.0, 25.0));
  EXPECT_FALSE(boxTarget->cast<SVGGraphicsElement>().resolvedSimpleClipPathOutline(
      /*maxVerbs=*/3, /*maxPoints=*/256, /*maxBytes=*/16u * 1024u));

  userTarget->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(10.0, 0.0));
  renderer.draw(document);
  const auto movedResource = userTarget->cast<SVGGraphicsElement>().resolvedSimpleClipPathOutline(
      /*maxVerbs=*/128, /*maxPoints=*/256, /*maxBytes=*/16u * 1024u);
  ASSERT_TRUE(movedResource.has_value());
  EXPECT_EQ(movedResource->bounds(), Box2d::FromXYWH(22.0, 9.0, 20.0, 24.0))
      << "The effective userSpaceOnUse clip follows the referencing element transform";
}

TEST(SVGClipPathElementTests, SimpleResolvedOutlineRejectsBooleanAndGroupObjectBoxes) {
  SVGDocument document = instantiateSubtree(R"svg(
    <defs>
      <clipPath id="many"><rect width="10" height="10"/><rect x="12" width="10" height="10"/></clipPath>
      <clipPath id="box" clipPathUnits="objectBoundingBox"><rect width="1" height="1"/></clipPath>
    </defs>
    <rect id="multi-target" width="30" height="30" clip-path="url(#many)"/>
    <g id="group-target" clip-path="url(#box)"><rect width="30" height="30"/></g>
  )svg",
                                            {}, Vector2i(120, 120));
  Renderer renderer;
  renderer.draw(document);
  const auto multi = document.querySelector("#multi-target");
  const auto group = document.querySelector("#group-target");
  ASSERT_TRUE(multi.has_value());
  ASSERT_TRUE(group.has_value());
  bool hasClipPath = false;
  EXPECT_FALSE(multi->cast<SVGGraphicsElement>().resolvedSimpleClipPathOutline(128, 256, 16384,
                                                                               &hasClipPath));
  EXPECT_TRUE(hasClipPath);
  EXPECT_FALSE(group->cast<SVGGraphicsElement>().resolvedSimpleClipPathOutline(128, 256, 16384));
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
        <rect width="16" height="16" clip-path="url(#a)" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matchBackend()
                  .geode(R"(
        .....@@@@@@.....
        ...@@@@@@@@@@...
        ..@@@@@@@@@@@@..
        .@@@@@@@@@@@@@@.
        .@@@@@@@@@@@@@@.
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        .@@@@@@@@@@@@@@.
        .@@@@@@@@@@@@@@.
        ..@@@@@@@@@@@@..
        ...@@@@@@@@@@...
        .....@@@@@@.....
        )")
                  .tinySkia(R"(
        ......@@@@......
        ....@@@@@@@@....
        ..@@@@@@@@@@@@..
        ..@@@@@@@@@@@@..
        .@@@@@@@@@@@@@@.
        .@@@@@@@@@@@@@@.
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@.
        .@@@@@@@@@@@@@@.
        ..@@@@@@@@@@@@..
        ..@@@@@@@@@@@@..
        ....@@@@@@@@....
        ......@@@@......
        )"));
}

TEST(SVGClipPathElementTests, RenderingObjectBoundingBox) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="a" clipPathUnits="objectBoundingBox">
          <circle cx="0.5" cy="0.5" r="0.5" />
        </clipPath>
        <rect width="8" height="8" clip-path="url(#a)" fill="black" />
        <rect y="8" width="16" height="8" clip-path="url(#a)" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ..@@@@..........
        .@@@@@@.........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        @@@@@@@@........
        .@@@@@@.........
        ..@@@@..........
        ....@@@@@@@@....
        ..@@@@@@@@@@@@..
        .@@@@@@@@@@@@@@.
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        .@@@@@@@@@@@@@@.
        ..@@@@@@@@@@@@..
        ....@@@@@@@@....
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
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
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
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@..@@@@..@@@@
  @@@@..@@@@..@@@@
  @@@@..@@@@..@@@@
  @@@@..@@@@..@@@@
  @@@@........@@@@
  @@@@........@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
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
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
  @......@@......@
  @......@@......@
  @.@@@@.@@......@
  @.@@@@.@@......@
  @.@@@@.@@......@
  @.@@@@.@@......@
  @.@@@@.@@......@
  @.@@@@.@@......@
  @.@@@@.@@......@
  @.@@@@.@@......@
  @......@@......@
  @......@@......@
  @@@@@@@@@@@@@@@@
  @@@@@@@@@@@@@@@@
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
        <rect width="16" height="16" clip-path="url(#clipTransform)" fill="black" />
        )-");

  // Expected output assumes that the translated circle defines a shifted clipping region.
  EXPECT_TRUE(generatedAscii.matchBackend()
                  .geode(R"(
        ................
        ................
        .......@@@@@@...
        .....@@@@@@@@@@.
        ....@@@@@@@@@@@@
        ...@@@@@@@@@@@@@
        ...@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ...@@@@@@@@@@@@@
        ...@@@@@@@@@@@@@
        ....@@@@@@@@@@@@
        )")
                  .tinySkia(R"(
        ................
        ................
        ........@@@@....
        ......@@@@@@@@..
        ....@@@@@@@@@@@@
        ....@@@@@@@@@@@@
        ...@@@@@@@@@@@@@
        ...@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ..@@@@@@@@@@@@@@
        ...@@@@@@@@@@@@@
        ...@@@@@@@@@@@@@
        ....@@@@@@@@@@@@
        ....@@@@@@@@@@@@
        )"));
}

/**
 * Verify that a clipPath with multiple child elements-each potentially having their own transforms-
 * is correctly applied when rendering.
 */
TEST(SVGClipPathElementTests, RenderingMultipleChildrenWithTransforms) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <clipPath id="clipMultiTrans">
          <circle cx="4" cy="4" r="4" transform="translate(4,0)" />
          <rect x="0" y="4" width="8" height="4" />
        </clipPath>
        <rect width="16" height="16" clip-path="url(#clipMultiTrans)" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ......@@@@......
        .....@@@@@@.....
        ....@@@@@@@@....
        ....@@@@@@@@....
        @@@@@@@@@@@@....
        @@@@@@@@@@@@....
        @@@@@@@@@@@.....
        @@@@@@@@@@......
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
 * Verify that a clipPath can reference a shape via a direct &lt;use&gt; child.
 */
TEST(SVGClipPathElementTests, RenderingUseChild) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <defs>
          <rect id="shapeRef" x="8" y="0" width="8" height="16" />
        </defs>
        <clipPath id="clipUse">
          <use href="#shapeRef" />
        </clipPath>
        <rect width="16" height="16" clip-path="url(#clipUse)" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
      ........@@@@@@@@
      ........@@@@@@@@
      ........@@@@@@@@
      ........@@@@@@@@
      ........@@@@@@@@
      ........@@@@@@@@
      ........@@@@@@@@
      ........@@@@@@@@
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

/**
 * Verify that an invisible &lt;use&gt; child does not contribute geometry to a clipPath.
 */
TEST(SVGClipPathElementTests, RenderingUseChildDisplayNone) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <defs>
          <rect id="shapeRef" x="0" y="0" width="16" height="16" />
        </defs>
        <clipPath id="clipUse">
          <use href="#shapeRef" display="none" />
        </clipPath>
        <rect width="16" height="16" clip-path="url(#clipUse)" fill="black" />
        )-");

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

}  // namespace donner::svg
