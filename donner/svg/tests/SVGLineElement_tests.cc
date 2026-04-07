#include "donner/svg/SVGLineElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGRectElement.h"  // For a negative cast test.
#include "donner/svg/core/tests/PathTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Ne;

namespace donner::svg {

using Command = Path::Command;
using CommandType = Path::Verb;

namespace {

// Helper matchers to compare the line's attributes.
auto X1Eq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x1", &SVGLineElement::x1, LengthIs(valueMatcher, unitMatcher));
}

auto Y1Eq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y1", &SVGLineElement::y1, LengthIs(valueMatcher, unitMatcher));
}

auto X2Eq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x2", &SVGLineElement::x2, LengthIs(valueMatcher, unitMatcher));
}

auto Y2Eq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y2", &SVGLineElement::y2, LengthIs(valueMatcher, unitMatcher));
}

/// Matcher that extracts the element (wrapped in a ParsedFragment) and applies a given matcher.
MATCHER_P(LineHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

MATCHER_P(ComputedSplineIs, matchers, "") {
  const auto& maybeSpline = arg.element.computedSpline();
  if (maybeSpline) {
    *result_listener << "computed spline is " << *maybeSpline;
    return testing::ExplainMatchResult(matchers, maybeSpline.value(), result_listener);
  } else {
    *result_listener << "spline is empty";
    return false;
  }
}

}  // namespace

TEST(SVGLineElementTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGLineElement>(  //
                  "<line />"),
              LineHas(AllOf(X1Eq(0.0, Lengthd::Unit::None),  //
                            Y1Eq(0.0, Lengthd::Unit::None),  //
                            X2Eq(0.0, Lengthd::Unit::None),  //
                            Y2Eq(0.0, Lengthd::Unit::None))));
}

TEST(SVGLineElementTests, Simple) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGLineElement>(  //
                  R"(<line x1="10" y1="20" x2="100" y2="50" />)"),
              LineHas(AllOf(X1Eq(10.0, Lengthd::Unit::None),   //
                            Y1Eq(20.0, Lengthd::Unit::None),   //
                            X2Eq(100.0, Lengthd::Unit::None),  //
                            Y2Eq(50.0, Lengthd::Unit::None))));
}

TEST(SVGLineElementTests, Units) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGLineElement>(  //
                  R"(<line x1="10px" y1="20%" x2="5em" y2="0" />)"),
              LineHas(AllOf(X1Eq(10.0, Lengthd::Unit::Px),        //
                            Y1Eq(20.0, Lengthd::Unit::Percent),   //
                            X2Eq(5.0, Lengthd::Unit::Em),         //
                            Y2Eq(0.0, Lengthd::Unit::None))));
}

TEST(SVGLineElementTests, Cast) {
  auto line = instantiateSubtreeElementAs<SVGLineElement>(R"(<line />)");
  EXPECT_THAT(line.element.tryCast<SVGElement>(), Ne(std::nullopt));
  EXPECT_THAT(line.element.tryCast<SVGGeometryElement>(), Ne(std::nullopt));
  EXPECT_THAT(line.element.tryCast<SVGLineElement>(), Ne(std::nullopt));
  // Ensure that an unrelated type (e.g. SVGRectElement) does not match.
  EXPECT_THAT(line.element.tryCast<SVGRectElement>(), Eq(std::nullopt));
}

/**
 * Test that updating the line's attributes via setters works and that subsequent
 * attribute getters reflect the changes.
 */
TEST(SVGLineElementTests, UpdateCoordinates) {
  auto fragment = instantiateSubtreeElementAs<SVGLineElement>(R"(
    <line x1="10" y1="20" x2="100" y2="50" />
  )");
  EXPECT_THAT(fragment->x1(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->y1(), LengthIs(20.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->x2(), LengthIs(100.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->y2(), LengthIs(50.0, Lengthd::Unit::None));

  // Update the coordinates.
  fragment->setX1(Lengthd(15, Lengthd::Unit::Px));
  fragment->setY1(Lengthd(25, Lengthd::Unit::Px));
  fragment->setX2(Lengthd(200, Lengthd::Unit::Em));
  fragment->setY2(Lengthd(75, Lengthd::Unit::Percent));

  EXPECT_THAT(fragment->x1(), LengthIs(15.0, Lengthd::Unit::Px));
  EXPECT_THAT(fragment->y1(), LengthIs(25.0, Lengthd::Unit::Px));
  EXPECT_THAT(fragment->x2(), LengthIs(200.0, Lengthd::Unit::Em));
  EXPECT_THAT(fragment->y2(), LengthIs(75.0, Lengthd::Unit::Percent));
}

/**
 * Test that setting coordinates on a default-constructed line works.
 */
TEST(SVGLineElementTests, SetFromDefaults) {
  auto fragment = instantiateSubtreeElementAs<SVGLineElement>("<line />");

  // All defaults should be zero.
  EXPECT_THAT(fragment->x1(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->y1(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->x2(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->y2(), LengthIs(0.0, Lengthd::Unit::None));

  // Set new values.
  fragment->setX1(Lengthd(5, Lengthd::Unit::None));
  fragment->setY1(Lengthd(10, Lengthd::Unit::None));
  fragment->setX2(Lengthd(50, Lengthd::Unit::None));
  fragment->setY2(Lengthd(100, Lengthd::Unit::None));

  EXPECT_THAT(fragment->x1(), LengthIs(5.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->y1(), LengthIs(10.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->x2(), LengthIs(50.0, Lengthd::Unit::None));
  EXPECT_THAT(fragment->y2(), LengthIs(100.0, Lengthd::Unit::None));
}

/**
 * Verify that computedSpline() produces a 2-point line segment (MoveTo + LineTo).
 */
TEST(SVGLineElementTests, ComputedSpline) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGLineElement>(  //
          R"(<line x1="10" y1="20" x2="100" y2="50" />)"),
      ComputedSplineIs(PointsAndCommandsAre(
          ElementsAre(Vector2d(10, 20), Vector2d(100, 50)),  //
          ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}))));
}

/**
 * Verify that a degenerate line (start == end) still produces a valid spline.
 */
TEST(SVGLineElementTests, ComputedSplineDegenerate) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGLineElement>(  //
          R"(<line x1="50" y1="50" x2="50" y2="50" />)"),
      ComputedSplineIs(PointsAndCommandsAre(
          ElementsAre(Vector2d(50, 50), Vector2d(50, 50)),  //
          ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}))));
}

/**
 * Verify that a default line (all zeros) produces no spline, since it is degenerate with no
 * visible geometry.
 */
TEST(SVGLineElementTests, ComputedSplineDefaults) {
  auto fragment = instantiateSubtreeElementAs<SVGLineElement>("<line />");
  EXPECT_THAT(fragment.element.computedSpline(), Eq(std::nullopt));
}

}  // namespace donner::svg
