#include "donner/svg/SVGEllipseElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGRadialGradientElement.h"  // used for a negative cast test
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::Eq;
using testing::Optional;

namespace donner::svg {

namespace {

// Helper matchers to compare the ellipse’s attributes.
auto CxEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("cx", &SVGEllipseElement::cx, LengthIs(valueMatcher, unitMatcher));
}

auto CyEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("cy", &SVGEllipseElement::cy, LengthIs(valueMatcher, unitMatcher));
}

auto RxIsAuto() {
  return testing::Property("rx", &SVGEllipseElement::rx, Eq(std::nullopt));
}

auto RxEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("rx", &SVGEllipseElement::rx,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

auto RyIsAuto() {
  return testing::Property("ry", &SVGEllipseElement::ry, Eq(std::nullopt));
}

auto RyEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("ry", &SVGEllipseElement::ry,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

/// Matcher that extracts the element (wrapped in a ParsedFragment) and applies a given matcher.
MATCHER_P(ElementHas, matcher, "") {
  return testing::ExplainMatchResult(matcher, arg.element, result_listener);
}

}  // namespace

TEST(SVGEllipseElementTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGEllipseElement>(  //
                  "<ellipse />"),
              ElementHas(AllOf(CxEq(0.0, Lengthd::Unit::None),  //
                               CyEq(0.0, Lengthd::Unit::None),  //
                               RxIsAuto(),                      //
                               RyIsAuto())));
}

TEST(SVGEllipseElementTests, Simple) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGEllipseElement>(  //
                  R"(<ellipse cx="50" cy="50" rx="40" ry="30" />)"),
              ElementHas(AllOf(CxEq(50.0, Lengthd::Unit::None),  //
                               CyEq(50.0, Lengthd::Unit::None),  //
                               RxEq(40.0, Lengthd::Unit::None),  //
                               RyEq(30.0, Lengthd::Unit::None))));
}

TEST(SVGEllipseElementTests, Units) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGEllipseElement>(  //
                  R"(<ellipse cx="50px" cy="30em" rx="0" />)"),
              ElementHas(AllOf(CxEq(50.0, Lengthd::Unit::Px),  //
                               CyEq(30.0, Lengthd::Unit::Em),  //
                               RxEq(0.0, Lengthd::Unit::None))));
}

TEST(SVGEllipseElementTests, PresentationAttributes) {
  auto result = instantiateSubtreeElementAs<SVGEllipseElement>(R"(
      <ellipse />
      <style>
        ellipse {
          cx: 0;
          cy: 10px;
          rx: 20em;
          ry: 30ex;
        }
      </style>
    )");

  // The computed values come from presentation (CSS) attributes.
  EXPECT_THAT(result.element.computedCx(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(result.element.computedCy(), LengthIs(10.0, Lengthd::Unit::Px));
  EXPECT_THAT(result.element.computedRx(), LengthIs(20.0, Lengthd::Unit::Em));
  EXPECT_THAT(result.element.computedRy(), LengthIs(30.0, Lengthd::Unit::Ex));

  // But the raw attributes (as set on the element) remain unchanged.
  EXPECT_THAT(result, ElementHas(AllOf(CxEq(0.0, Lengthd::Unit::None),
                                       CyEq(0.0, Lengthd::Unit::None), RxIsAuto(), RyIsAuto())));
}

/// Verify that the ellipse can be safely down‐cast to the appropriate base types.
TEST(SVGEllipseElementTests, Cast) {
  auto ellipse = instantiateSubtreeElementAs<SVGEllipseElement>("<ellipse />");
  EXPECT_THAT(ellipse->tryCast<SVGElement>(), testing::Ne(std::nullopt));
  EXPECT_THAT(ellipse->tryCast<SVGGeometryElement>(), testing::Ne(std::nullopt));
  EXPECT_THAT(ellipse->tryCast<SVGEllipseElement>(), testing::Ne(std::nullopt));
  // Ensure that an unrelated type (e.g. SVGRadialGradientElement) does not match.
  EXPECT_THAT(ellipse->tryCast<SVGRadialGradientElement>(), testing::Eq(std::nullopt));
}

/// Test that updating attributes via setters works and that subsequent attribute getters reflect
/// the changes.
TEST(SVGEllipseElementTests, UpdateCoordinates) {
  auto fragment = instantiateSubtreeElementAs<SVGEllipseElement>(R"(
    <ellipse cx="10" cy="20" rx="30" ry="40" />
  )");
  EXPECT_THAT(fragment->cx(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->cy(), LengthIs(20.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->rx(), Optional(LengthIs(30.0, Lengthd::Unit::None)));
  EXPECT_THAT(fragment->ry(), Optional(LengthIs(40.0, Lengthd::Unit::None)));

  // Update the coordinates.
  fragment->setCx(Lengthd(15, Lengthd::Unit::Px));
  fragment->setCy(Lengthd(25, Lengthd::Unit::Px));
  fragment->setRx(Lengthd(35, Lengthd::Unit::Px));
  fragment->setRy(Lengthd(45, Lengthd::Unit::Px));

  EXPECT_THAT(fragment->cx(), LengthIs(15.0, Lengthd::Unit::Px));
  EXPECT_THAT(fragment->cy(), LengthIs(25.0, Lengthd::Unit::Px));
  EXPECT_THAT(fragment->rx(), Optional(LengthIs(35.0, Lengthd::Unit::Px)));
  EXPECT_THAT(fragment->ry(), Optional(LengthIs(45.0, Lengthd::Unit::Px)));
}

/// Test the "auto" fallback behavior: if only one radius is specified then the computed value for
/// the other should match.
TEST(SVGEllipseElementTests, ComputedValuesFallback) {
  {  // Only rx provided – computed ry should fall back to the same value.
    auto fragment = instantiateSubtreeElementAs<SVGEllipseElement>(R"(
      <ellipse cx="100" cy="100" rx="50" />
    )");
    EXPECT_THAT(fragment->rx(), Optional(LengthIs(50.0, Lengthd::Unit::None)));
    EXPECT_THAT(fragment->ry(), Eq(std::nullopt));
    EXPECT_THAT(fragment->computedRx(), LengthIs(50.0, Lengthd::Unit::None));
    EXPECT_THAT(fragment->computedRy(), LengthIs(50.0, Lengthd::Unit::None));
  }

  {  // Only ry provided – computed rx should fall back.
    auto fragment = instantiateSubtreeElementAs<SVGEllipseElement>(R"(
      <ellipse cx="100" cy="100" ry="60" />
    )");
    EXPECT_THAT(fragment->ry(), Optional(LengthIs(60.0, Lengthd::Unit::None)));
    EXPECT_THAT(fragment->rx(), Eq(std::nullopt));
    EXPECT_THAT(fragment->computedRy(), LengthIs(60.0, Lengthd::Unit::None));
    EXPECT_THAT(fragment->computedRx(), LengthIs(60.0, Lengthd::Unit::None));
  }
}

/// Verify that presentation (CSS) attributes override the element’s raw attribute values when
/// computing the final (computed) values.
TEST(SVGEllipseElementTests, ComputedValuesOverrideAttributes) {
  auto result = instantiateSubtreeElementAs<SVGEllipseElement>(R"(
      <ellipse cx="20" cy="30" rx="40" ry="50" />
      <style>
        ellipse {
          cx: 100;
          ry: 200;
        }
      </style>
    )");

  // The raw attribute values remain unchanged.
  EXPECT_THAT(result.element.cx(), LengthIs(20, Lengthd::Unit::None));
  EXPECT_THAT(result.element.cy(), LengthIs(30, Lengthd::Unit::None));
  EXPECT_THAT(result.element.rx(), Optional(LengthIs(40, Lengthd::Unit::None)));
  EXPECT_THAT(result.element.ry(), Optional(LengthIs(50, Lengthd::Unit::None)));

  // The computed values use the presentation values where provided.
  EXPECT_THAT(result.element.computedCx(), LengthIs(100, Lengthd::Unit::None));
  EXPECT_THAT(result.element.computedCy(),
              LengthIs(30, Lengthd::Unit::None));  // cy falls back to the raw value.
  EXPECT_THAT(result.element.computedRx(), LengthIs(40, Lengthd::Unit::None));
  EXPECT_THAT(result.element.computedRy(), LengthIs(200, Lengthd::Unit::None));
}

/// Verify that an ellipse element is rendered as expected.
TEST(SVGEllipseElementTests, Rendering) {
  ParsedFragment<SVGEllipseElement> fragment = instantiateSubtreeElementAs<SVGEllipseElement>(R"(
    <ellipse id="e" cx="8" cy="8" rx="6" ry="4" fill="white" />
  )");

  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
  EXPECT_TRUE(generatedAscii.matches(R"(
      ................
      ................
      ................
      ................
      .....@@@@@@.....
      ...@@@@@@@@@@...
      ..@@@@@@@@@@@@..
      ..@@@@@@@@@@@@..
      ..@@@@@@@@@@@@..
      ..@@@@@@@@@@@@..
      ...@@@@@@@@@@...
      .....@@@@@@.....
      ................
      ................
      ................
      ................
      )"));
}

/// Verify that an ellipse element with stroke only is rendered as expected.
/// (This test uses an ellipse with no fill and a white stroke.)
TEST(SVGEllipseElementTests, RenderingStroke) {
  ParsedFragment<SVGEllipseElement> fragment = instantiateSubtreeElementAs<SVGEllipseElement>(R"(
    <ellipse cx="8" cy="8" rx="4" ry="8" fill="none" stroke="white" stroke-width="1" />
  )");

  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
  EXPECT_TRUE(generatedAscii.matches(R"(
      ......@@@@......
      .....@....@.....
      .....@....@.....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      .....@....@.....
      .....@....@.....
      ......@@@@......
  )"));
}

/// Verify that an ellipse element with a transform is rendered as expected.
/// (This test applies a translation and a rotation to the ellipse.)
TEST(SVGEllipseElementTests, RenderingTransform) {
  ParsedFragment<SVGEllipseElement> fragment = instantiateSubtreeElementAs<SVGEllipseElement>(R"-(
    <ellipse cx="8" cy="8" rx="2" ry="6" fill="white" transform="translate(8 8) rotate(45) translate(-8 -8)" />
    )-");

  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
  EXPECT_TRUE(generatedAscii.matches(R"(
      ................
      ................
      ................
      ................
      .........@@@....
      ........@@@@....
      .......@@@@@....
      ......@@@@@.....
      .....@@@@@......
      ....@@@@@.......
      ....@@@@........
      ....@@@.........
      ................
      ................
      ................
      ................
  )"));
}

}  // namespace donner::svg
