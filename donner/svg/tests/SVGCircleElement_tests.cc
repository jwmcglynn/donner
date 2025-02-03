#include "donner/svg/SVGCircleElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGEllipseElement.h"  // For a negative cast test.
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::Eq;
using testing::Ne;

namespace donner::svg {

namespace {

// Helper matchers to compare the circle’s attributes.
auto CxEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("cx", &SVGCircleElement::cx, LengthIs(valueMatcher, unitMatcher));
}

auto CyEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("cy", &SVGCircleElement::cy, LengthIs(valueMatcher, unitMatcher));
}

auto REq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("r", &SVGCircleElement::r, LengthIs(valueMatcher, unitMatcher));
}

/// Matcher that extracts the element (wrapped in a ParsedFragment) and applies a given matcher.
MATCHER_P(CircleHas, matcher, "") {
  return testing::ExplainMatchResult(matcher, arg.element, result_listener);
}

}  // namespace

TEST(SVGCircleElementTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGCircleElement>(  //
                  "<circle />"),
              CircleHas(AllOf(CxEq(0.0, Lengthd::Unit::None),  //
                              CyEq(0.0, Lengthd::Unit::None),  //
                              REq(0.0, Lengthd::Unit::None))));
}

TEST(SVGCircleElementTests, Simple) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGCircleElement>(  //
                  R"(<circle cx="50" cy="50" r="40" />)"),
              CircleHas(AllOf(CxEq(50.0, Lengthd::Unit::None),  //
                              CyEq(50.0, Lengthd::Unit::None),  //
                              REq(40.0, Lengthd::Unit::None))));
}

TEST(SVGCircleElementTests, Cast) {
  auto circle = instantiateSubtreeElementAs<SVGCircleElement>(R"(<circle />)");
  EXPECT_THAT(circle.element.tryCast<SVGElement>(), Ne(std::nullopt));
  EXPECT_THAT(circle.element.tryCast<SVGGeometryElement>(), Ne(std::nullopt));
  EXPECT_THAT(circle.element.tryCast<SVGCircleElement>(), Ne(std::nullopt));
  // Ensure that an unrelated type (e.g. SVGEllipseElement) does not match.
  EXPECT_THAT(circle.element.tryCast<SVGEllipseElement>(), Eq(std::nullopt));
}

TEST(SVGCircleElementTests, Units) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGCircleElement>(  //
                  R"(<circle cx="50px" cy="30em" r="0" />)"),
              CircleHas(AllOf(CxEq(50.0, Lengthd::Unit::Px),  //
                              CyEq(30.0, Lengthd::Unit::Em),  //
                              REq(0.0, Lengthd::Unit::None))));
}

TEST(SVGCircleElementTests, PresentationAttributes) {
  auto result = instantiateSubtreeElementAs<SVGCircleElement>(R"(
      <circle />
      <style>
        circle {
          cx: 0;
          cy: 10px;
          r: 20em;
        }
      </style>
    )");

  // The computed values come from presentation (CSS) attributes.
  EXPECT_THAT(result.element.computedCx(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(result.element.computedCy(), LengthIs(10.0, Lengthd::Unit::Px));
  EXPECT_THAT(result.element.computedR(), LengthIs(20.0, Lengthd::Unit::Em));

  // But the raw attributes (as set on the element) remain unchanged.
  EXPECT_THAT(result,
              CircleHas(AllOf(CxEq(0.0, Lengthd::Unit::None), CyEq(0.0, Lengthd::Unit::None),
                              REq(0.0, Lengthd::Unit::None))));
}

/**
 * Test that updating the circle's attributes via setters works and that subsequent
 * attribute getters reflect the changes.
 */
TEST(SVGCircleElementTests, UpdateCoordinates) {
  auto fragment = instantiateSubtreeElementAs<SVGCircleElement>(R"(
    <circle cx="10" cy="20" r="30" />
  )");
  EXPECT_THAT(fragment->cx(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->cy(), LengthIs(20.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->r(), LengthIs(30.0, Lengthd::Unit::None));

  // Update the coordinates.
  fragment->setCx(Lengthd(15, Lengthd::Unit::Px));
  fragment->setCy(Lengthd(25, Lengthd::Unit::Px));
  fragment->setR(Lengthd(35, Lengthd::Unit::Px));

  EXPECT_THAT(fragment->cx(), LengthIs(15.0, Lengthd::Unit::Px));
  EXPECT_THAT(fragment->cy(), LengthIs(25.0, Lengthd::Unit::Px));
  EXPECT_THAT(fragment->r(), LengthIs(35.0, Lengthd::Unit::Px));
}

/**
 * Verify that presentation (CSS) attributes override the element’s raw attribute values
 * when computing the final (computed) values.
 */
TEST(SVGCircleElementTests, ComputedValuesOverrideAttributes) {
  auto result = instantiateSubtreeElementAs<SVGCircleElement>(R"(
      <circle cx="20" cy="30" r="40" />
      <style>
        circle {
          cx: 100;
          r: 200;
        }
      </style>
    )");

  // The raw attribute values remain unchanged.
  EXPECT_THAT(result.element.cx(), LengthIs(20, Lengthd::Unit::None));
  EXPECT_THAT(result.element.cy(), LengthIs(30, Lengthd::Unit::None));
  EXPECT_THAT(result.element.r(), LengthIs(40, Lengthd::Unit::None));

  // The computed values use the presentation values where provided.
  EXPECT_THAT(result.element.computedCx(), LengthIs(100, Lengthd::Unit::None));
  EXPECT_THAT(result.element.computedCy(),
              LengthIs(30, Lengthd::Unit::None));  // No override for cy.
  EXPECT_THAT(result.element.computedR(), LengthIs(200, Lengthd::Unit::None));
}

/**
 * Verify that a circle element is rendered as expected.
 */
TEST(SVGCircleElementTests, Rendering) {
  ParsedFragment<SVGCircleElement> fragment = instantiateSubtreeElementAs<SVGCircleElement>(R"(
    <circle cx="8" cy="8" r="6" fill="white" />
  )");

  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
  EXPECT_TRUE(generatedAscii.matches(R"(
      ................
      ................
      ......@@@@......
      ....@@@@@@@@....
      ...@@@@@@@@@@...
      ...@@@@@@@@@@...
      ..@@@@@@@@@@@@..
      ..@@@@@@@@@@@@..
      ..@@@@@@@@@@@@..
      ..@@@@@@@@@@@@..
      ...@@@@@@@@@@...
      ...@@@@@@@@@@...
      ....@@@@@@@@....
      ......@@@@......
      ................
      ................
  )"));
}

/**
 * Verify that a circle element with stroke only is rendered as expected.
 * (This test uses a circle with no fill and a white stroke.)
 */
TEST(SVGCircleElementTests, RenderingStroke) {
  ParsedFragment<SVGCircleElement> fragment = instantiateSubtreeElementAs<SVGCircleElement>(R"(
    <circle cx="8" cy="8" r="4" fill="none" stroke="white" stroke-width="1" />
  )");

  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(fragment.document);
  EXPECT_TRUE(generatedAscii.matches(R"(
      ................
      ................
      ................
      ................
      .....@@@@@@.....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      ....@......@....
      .....@@@@@@.....
      ................
      ................
      ................
      ................
  )"));
}

}  // namespace donner::svg
