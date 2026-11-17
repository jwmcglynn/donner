#include "donner/svg/SVGPatternElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {

TEST(SVGPatternElementTests, Defaults) {
  auto pattern = instantiateSubtreeElementAs<SVGPatternElement>("<pattern />");

  EXPECT_THAT(pattern->viewBox(), testing::Eq(std::nullopt));
  EXPECT_THAT(pattern->preserveAspectRatio(),
              testing::Eq(PreserveAspectRatio{PreserveAspectRatio::Align::XMidYMid,
                                              PreserveAspectRatio::MeetOrSlice::Meet}));

  EXPECT_THAT(pattern->x(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(pattern->y(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(pattern->width(), testing::Eq(std::nullopt));
  EXPECT_THAT(pattern->height(), testing::Eq(std::nullopt));

  EXPECT_THAT(pattern->patternUnits(), testing::Eq(PatternUnits::ObjectBoundingBox));
  EXPECT_THAT(pattern->patternContentUnits(), testing::Eq(PatternContentUnits::UserSpaceOnUse));
  EXPECT_THAT(pattern->patternTransform(), TransformEq(Transform2d()));
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

TEST(SVGPatternElementTests, SetViewBoxPreserveAspectRatioAndHref) {
  SVGDocument document;
  SVGPatternElement pattern = SVGPatternElement::Create(document);

  const Box2d viewBox(Vector2d(1, 2), Vector2d(11, 22));
  pattern.setViewBox(viewBox);
  pattern.setPreserveAspectRatio(PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMax,
                                                     PreserveAspectRatio::MeetOrSlice::Slice});
  pattern.setHref(RcStringOrRef("#base"));

  EXPECT_THAT(pattern.viewBox(), testing::Optional(testing::Eq(viewBox)));
  EXPECT_EQ(pattern.preserveAspectRatio(),
            (PreserveAspectRatio{PreserveAspectRatio::Align::XMinYMax,
                                 PreserveAspectRatio::MeetOrSlice::Slice}));
  EXPECT_THAT(pattern.href(), testing::Optional(testing::Eq("#base")));

  pattern.setHref(std::nullopt);
  EXPECT_EQ(pattern.href(), std::nullopt);
}

TEST(SVGPatternElementTests, ObjectBoundingBoxRendering) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" width="1" height="1">
          <circle r="4" cx="4" cy="4" fill="lime" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matchBackend()
                  .defaultPattern(R"(
        ..:**:..........
        .******.........
        :******:........
        ********........
        ********........
        =******:........
        .******.........
        ..:**:..........
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        )")
                  .geode(R"(
        .,=**=,.........
        ,******,........
        =******=........
        ********........
        ********........
        =******=........
        ,******,........
        .,=**=,.........
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
        ****....****....
        ****....****....
        ****....****....
        ****....****....
        ................
        ................
        ................
        ................
        ****....****....
        ****....****....
        ****....****....
        ****....****....
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
        ..****....****..
        ..****....****..
        ..****....****..
        ..****....****..
        ................
        ................
        ................
        ................
        ..****....****..
        ..****....****..
        ..****....****..
        ..****....****..
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
        ****....****....
        ****....****....
        ****....****....
        ****....****....
        ....====....====
        ....====....====
        ....====....====
        ....====....====
        ****....****....
        ****....****....
        ****....****....
        ****....****....
        ....====....====
        ....====....====
        ....====....====
        ....====....====
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
        ==....==....==..
        ==....==....==..
        ..****..****..**
        ..****..****..**
        ..****..****..**
        ..****..****..**
        ==....==....==..
        ==....==....==..
        ..****..****..**
        ..****..****..**
        ..****..****..**
        ..****..****..**
        ==....==....==..
        ==....==....==..
        ..****..****..**
        ..****..****..**
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
        ****....****....
        ****....****....
        **====..**====..
        **====..**====..
        ..====....====..
        ..====....====..
        ................
        ................
        ****....****....
        ****....****....
        **====..**====..
        **====..**====..
        ..====....====..
        ..====....====..
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
        ****************
        ****************
        **==**==**==**==
        **==**==**==**==
        ****************
        ****************
        **==**==**==**==
        **==**==**==**==
        ****************
        ****************
        **==**==**==**==
        **==**==**==**==
        ****************
        ****************
        **==**==**==**==
        **==**==**==**==
        )"));
}

TEST(SVGPatternElementTests, PatternTransform) {
  auto pattern = instantiateSubtreeElementAs<SVGPatternElement>(R"-(
    <pattern patternTransform="scale(2)" />
  )-");

  EXPECT_THAT(pattern->patternTransform(), TransformIs(2, 0, 0, 2, 0, 0));

  pattern->setPatternTransform(Transform2d::Translate(Vector2d(3.0, 5.0)));
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
        :*******:.......
        .:*******:......
        ..:*******:.....
        ...:*******:....
        ....:*******:...
        .....:*******:..
        ......:*******:.
        .......:*******:
        -=======-.......
        .-=======-......
        ..-=======-.....
        ...-=======-....
        ....-=======-...
        .....-=======-..
        ......-=======-.
        .......-=======-
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
        -....*......****
        =-..***......**-
        ==-*****......-.
        ==.:*****....-=.
        =...:*****..-===
        .....:*****-====
        *.....:***-.====
        **.....:*-...===
        ***.....-.....==
        ****...-=-.....=
        *****.-===-.....
        *****-=====-....
        .***-..=====-...
        ..*-....=====-.*
        ..-......=====:*
        .-==......===..:
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
        :*******:.......
        .:*******:......
        ..:*******:.....
        ...:*******:....
        ....:*******:...
        .....:*******:..
        ......:*******:.
        .......:*******:
        -=======-.......
        .-=======-......
        ..-=======-.....
        ...-=======-....
        ....-=======-...
        .....-=======-..
        ......-=======-.
        .......-=======-
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
        ====-.......-===
        =====-.......-==
        ======-.......-=
        =======-.......-
        :.......:*******
        *:.......:******
        **:.......:*****
        ***:.......:****
        ****:.......:***
        *****:.......:**
        ******:.......:*
        *******:.......:
        -.......-=======
        =-.......-======
        ==-.......-=====
        ===-.......-====
        )"));
}

TEST(SVGPatternElementTests, PatternRecursionOnChild) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" width="1" height="1">
          <circle r="4" cx="4" cy="4" fill="url(#a)" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
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

/**
 * Mutual recursion through pattern content: `a`'s rect fills with `b`, and `b`'s rect fills with
 * `a`. Only the reference that revisits a pattern already being rendered is invalid (SVG 2,
 * Linking: circular references; Painting: an invalid paint server paints nothing for that layer).
 * So `a`'s tile paints `b`'s tile, whose rect keeps its stroke and drops only its fill. Chromium
 * and Firefox render resvg's paint-servers/pattern/recursive-on-child the same way.
 */
TEST(SVGPatternElementTests, PatternMutualRecursionDropsOnlyTheCycleClosingLayer) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#b)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black" stroke-width="2" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        )"));
}

/**
 * The cycle-closing layer is the stroke here: `b`'s rect keeps its black fill and drops only the
 * stroke that references `a`.
 */
TEST(SVGPatternElementTests, PatternMutualRecursionDropsACycleClosingStroke) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#b)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect x="2" y="2" width="4" height="4" fill="black" stroke="url(#a)" stroke-width="2" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ................
        ................
        ................
        ................
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ................
        ................
        )"));
}

/**
 * The cycle also closes through `href` templating: `c` has no content of its own and inherits
 * `b`'s, whose fill references `a` again.
 */
TEST(SVGPatternElementTests, PatternMutualRecursionThroughHrefTemplate) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#c)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black" stroke-width="2" />
        </pattern>
        <pattern id="c" xlink:href="#b" />
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        )"));
}

/**
 * Drawing the same document again gives the same result. Each draw builds a new driver, so this
 * covers retained document state; PatternMutualRecursionChainResetsBetweenElements covers the
 * driver's own chain within one draw.
 */
TEST(SVGPatternElementTests, PatternMutualRecursionDocumentRendersTheSameForEachDraw) {
  SVGDocument document = instantiateSubtree(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#b)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black" stroke-width="2" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  constexpr std::string_view kRing = R"(
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        )";
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(kRing));
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(kRing));
}
/**
 * Two elements painted with the same cyclic pattern in one draw: the active chain is empty again
 * after the first, so the second paints the same tile instead of nothing.
 */
TEST(SVGPatternElementTests, PatternMutualRecursionChainResetsBetweenElements) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#b)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect id="inner" x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black"
                stroke-width="2" />
        </pattern>
        <rect width="8" height="16" fill="url(#a)" />
        <rect x="8" width="8" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        )"));
}

/**
 * The cycle-closing reference has a fallback color, which an invalid paint server reference
 * paints instead (SVG 2 Painting, "Specifying paint").
 */
TEST(SVGPatternElementTests, PatternMutualRecursionPaintsTheFallbackColor) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#b)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect x="2" y="2" width="4" height="4" fill="url(#a) black" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ................
        ................
        ................
        ................
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ..@@@@....@@@@..
        ................
        ................
        )"));
}

/**
 * The cycle closes through a `<use>` inside the pattern content: the used rect's fill reference to
 * `a` revisits the active pattern, so only that layer is dropped.
 */
TEST(SVGPatternElementTests, PatternMutualRecursionThroughUse) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <defs>
          <rect id="ring" x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black"
                stroke-width="2" />
        </defs>
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <use href="#ring" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        )"));
}

/**
 * A `<use>` of an element inside a cyclic pattern draws the same picture as that element written
 * out directly: the shared offscreen subtree is not cut short while it is still being built.
 */
TEST(SVGPatternElementTests, PatternMutualRecursionUseOfPatternContentMatchesDirectElement) {
  const AsciiImage viaUse = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#b)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect id="inner" x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black"
                stroke-width="2" />
        </pattern>
        <use href="#inner" />
        <use href="#inner" x="8" y="8" />
        )-");
  const AsciiImage direct = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#b)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect id="inner" x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black"
                stroke-width="2" />
        </pattern>
        <rect x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black" stroke-width="2" />
        <rect x="10" y="10" width="4" height="4" fill="url(#a)" stroke="black" stroke-width="2" />
        )-");
  EXPECT_EQ(viaUse.generated, direct.generated);
}

/**
 * The cycle runs through an `feImage` fragment. The fragment renders in its own driver, which
 * starts with an empty pattern chain, so the feImage fragment guard (a fragment already being
 * rendered is skipped) and its 32-level depth cap bound it instead. Drawing must finish.
 */
TEST(SVGPatternElementTests, PatternRecursionThroughFeImageTerminates) {
  SVGDocument document = instantiateSubtree(R"-(
        <defs>
          <filter id="f" x="0" y="0" width="1" height="1">
            <feImage href="#r" />
          </filter>
          <rect id="r" width="8" height="8" fill="url(#a)" stroke="black" stroke-width="2" />
        </defs>
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="black" filter="url(#f)" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(document);
  EXPECT_THAT(generatedAscii.generated, testing::Not(testing::IsEmpty()));
}

/**
 * The chain is keyed by document as well as entity: a sub-document drawn inside a pattern tile has
 * its own entity ids, which can equal the host's active pattern. The sub-document's own patterns
 * must still paint. Its chain of patterns covers a run of entity ids, and the host's pattern `a`
 * sits inside that run, so one of them shares its id; the leading `<g>` shifts the run by one id
 * so both parities are covered. Each level is scaled by half so every tile has the same raster
 * size.
 */
TEST(SVGPatternElementTests, PatternChainIsPerDocument) {
  for (const bool shift : {false, true}) {
    std::string sub = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>";
    if (shift) {
      sub += "<g/>";
    }
    constexpr int kChainLength = 24;
    for (int i = 0; i < kChainLength; ++i) {
      // Halving each level keeps every tile's raster the same size, so the chain is not cut off by
      // the tile raster limit.
      sub += "<pattern id='p" + std::to_string(i) +
             "' patternUnits='userSpaceOnUse' width='16' height='16'" +
             (i == 0 ? std::string() : std::string(" patternTransform='scale(0.5)'")) +
             "><rect width='16' height='16'";
      sub += i + 1 < kChainLength ? " fill='url(%23p" + std::to_string(i + 1) + ")'" : "";
      sub += "/></pattern>";
    }
    sub += "<rect width='16' height='16' fill='url(%23p0)'/></svg>";
    std::string uri = "data:image/svg+xml,";
    for (const char c : sub) {
      uri += c == ' '   ? std::string("%20")
             : c == '<' ? std::string("%3C")
             : c == '>' ? std::string("%3E")
                        : std::string(1, c);
    }
    // Twenty empty groups first give the host pattern an id inside the sub-document's run.
    std::string svg;
    for (int i = 0; i < 20; ++i) {
      svg += "<g />";
    }
    svg +=
        "<pattern id=\"a\" patternUnits=\"userSpaceOnUse\" width=\"16\" height=\"16\">"
        "<image width=\"16\" height=\"16\" href=\"" +
        uri +
        "\" /></pattern>"
        "<rect width=\"16\" height=\"16\" fill=\"url(#a)\" />";
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(svg);

    EXPECT_TRUE(generatedAscii.matches(R"(
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        @@@@@@@@@@@@@@@@
        )"))
        << "shift=" << shift;
  }
}

/**
 * A three-pattern cycle still terminates: the chain a -> b -> c renders, and c's reference back to
 * `a` paints nothing because `a` is already being rendered.
 */
TEST(SVGPatternElementTests, PatternThreeWayRecursionTerminates) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#b)" />
        </pattern>
        <pattern id="b" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="url(#c)" />
        </pattern>
        <pattern id="c" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect x="2" y="2" width="4" height="4" fill="url(#a)" stroke="black" stroke-width="2" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        ................
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        .@@..@@..@@..@@.
        .@@..@@..@@..@@.
        .@@@@@@..@@@@@@.
        .@@@@@@..@@@@@@.
        ................
        )"));
}

/**
 * A long acyclic chain of pattern references (each pattern's content filled with the next) is not a
 * cycle, so the active-chain guard never stops it; the shadow-tree resource budget bounds its
 * expansion. It must finish and render the same way twice.
 */
TEST(SVGPatternElementTests, PatternLongAcyclicChainTerminates) {
  std::string svg;
  constexpr int kChainLength = 100;
  for (int i = 0; i < kChainLength; ++i) {
    svg += "<pattern id=\"p" + std::to_string(i) +
           "\" patternUnits=\"userSpaceOnUse\" width=\"8\" height=\"8\">";
    if (i + 1 < kChainLength) {
      svg += "<rect width=\"8\" height=\"8\" fill=\"url(#p" + std::to_string(i + 1) + ")\" />";
    } else {
      svg += "<rect x=\"2\" y=\"2\" width=\"4\" height=\"4\" fill=\"black\" />";
    }
    svg += "</pattern>";
  }
  svg += "<rect width=\"16\" height=\"16\" fill=\"url(#p0)\" />";

  SVGDocument document = instantiateSubtree(svg);
  const AsciiImage first = RendererTestUtils::renderToAsciiImage(document);
  const AsciiImage second = RendererTestUtils::renderToAsciiImage(document);
  EXPECT_EQ(first.generated, second.generated);
}

/**
 * Binary fan-out: each pattern's tile holds two rects filled with the next pattern, scaled by half
 * so every tile has the same raster size. Rendering it fully would draw 2^20 tiles. Every tile
 * reserves from the per-frame surface budget (256 surfaces, 256 MiB): the CPU backend keeps each
 * tile's reservation until the frame ends, and Geode reserves through the same budget when it
 * acquires tile textures. The budget latches (measured from 10 levels on both backends) and the
 * frame finishes.
 */
TEST(SVGPatternElementTests, PatternFanOutLatchesTheSurfaceBudget) {
  constexpr int kDepth = 20;
  std::string svg;
  for (int i = 0; i < kDepth; ++i) {
    svg += "<pattern id=\"p" + std::to_string(i) +
           "\" patternUnits=\"userSpaceOnUse\" width=\"8\" height=\"8\"" +
           (i == 0 ? std::string() : std::string(" patternTransform=\"scale(0.5)\"")) + ">";
    if (i + 1 < kDepth) {
      const std::string next = "url(#p" + std::to_string(i + 1) + ")";
      svg += "<rect width=\"4\" height=\"8\" fill=\"" + next + "\" />";
      svg += "<rect x=\"4\" width=\"4\" height=\"8\" fill=\"" + next + "\" />";
    } else {
      svg += "<rect width=\"8\" height=\"8\" fill=\"black\" />";
    }
    svg += "</pattern>";
  }
  svg += "<rect width=\"16\" height=\"16\" fill=\"url(#p0)\" />";

  SVGDocument document = instantiateSubtree(svg);
  std::unique_ptr<RendererInterface> renderer = CreateActiveRendererInstance();
  renderer->draw(document);
  EXPECT_TRUE(renderer->resourceStats().surfaceBudgetRejected);
}

/**
 * A pattern whose tile is skipped must not draw any of its content. `z` has a zero-width tile, so
 * it paints nothing; its rect is filled with `n`, whose tile content is instantiated after `z`'s
 * last element, and must not be drawn as ordinary content by the enclosing traversal.
 */
TEST(SVGPatternElementTests, SkippedZeroSizeTileDrawsNoNestedContent) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="black" />
        </pattern>
        <pattern id="z" patternUnits="userSpaceOnUse" width="0" height="8">
          <rect width="8" height="8" fill="url(#n)" />
        </pattern>
        <rect width="16" height="16" fill="url(#z)" />
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

/**
 * The same for a tile skipped because the per-frame surface budget has latched: 300 tiles of an
 * empty pattern use up the CPU backend's 256 surfaces first, so the last rect's tile is skipped and
 * its nested `n` content must not be drawn.
 */
TEST(SVGPatternElementTests, SkippedLatchedTileDrawsNoNestedContent) {
  if (!RendererTestUtils::IsTinySkiaBackend()) {
    GTEST_SKIP() << "the CPU backend keeps each tile's surface reservation until the frame ends";
  }
  std::string svg =
      "<pattern id='empty' patternUnits='userSpaceOnUse' width='8' height='8'>"
      "<rect width='8' height='8' fill='none' /></pattern>"
      "<pattern id='n' patternUnits='userSpaceOnUse' width='8' height='8'>"
      "<rect width='8' height='8' fill='black' /></pattern>"
      "<pattern id='p' patternUnits='userSpaceOnUse' width='8' height='8'>"
      "<rect width='8' height='8' fill='url(#n)' /></pattern>";
  for (int i = 0; i < 300; ++i) {
    svg += "<rect width='16' height='16' fill='url(#empty)' />";
  }
  svg += "<rect width='16' height='16' fill='url(#p)' />";

  SVGDocument document = instantiateSubtree(svg);
  std::unique_ptr<RendererInterface> renderer = CreateActiveRendererInstance();
  renderer->draw(document);
  ASSERT_TRUE(renderer->resourceStats().surfaceBudgetRejected);
  EXPECT_TRUE(RendererTestUtils::renderToAsciiImage(document).matches(R"(
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
 * Mask content is skipped when the masked element has empty bounds. A pattern-filled element in
 * that mask content must not have its pattern's content drawn as ordinary content either.
 */
TEST(SVGPatternElementTests, SkippedMaskContentDrawsNoNestedPatternContent) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="8" height="8">
          <rect width="8" height="8" fill="black" />
        </pattern>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="16" height="16">
          <rect width="8" height="8" fill="url(#n)" />
        </mask>
        <g mask="url(#m)" />
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

/**
 * Self-recursive still renders and should not crash.
 */
TEST(SVGPatternElementTests, PatternRecursionHref) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="a" width="1" height="1" xlink:href="#a">
          <circle r="4" cx="4" cy="4" fill="black" />
        </pattern>
        <rect width="16" height="16" fill="url(#a)" />
        )-");

  EXPECT_TRUE(generatedAscii.matchBackend()
                  .defaultPattern(R"(
        ..=@@=..........
        .@@@@@@.........
        =@@@@@@=........
        @@@@@@@@........
        @@@@@@@@........
        #@@@@@@=........
        .@@@@@@.........
        ..=@@=..........
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        )")
                  .geode(R"(
        .-#@@#-.........
        -@@@@@@-........
        #@@@@@@#........
        @@@@@@@@........
        @@@@@@@@........
        #@@@@@@#........
        -@@@@@@-........
        .-#@@#-.........
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

// TODO viewBox/preserveAspectRatio
// TODO: Additional tests and rendering tests

}  // namespace donner::svg
