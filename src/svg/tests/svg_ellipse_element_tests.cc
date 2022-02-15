#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/svg_ellipse_element.h"
#include "src/svg/tests/xml_test_utils.h"

using testing::AllOf;
using testing::Eq;
using testing::Optional;

namespace donner::svg {

MATCHER_P2(LengthIs, valueMatcher, unitMatcher, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.unit, result_listener);
}

namespace {

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

MATCHER_P(ElementHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
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

  EXPECT_THAT(result.element.computedCx(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(result.element.computedCy(), LengthIs(10.0, Lengthd::Unit::Px));
  EXPECT_THAT(result.element.computedRx(), LengthIs(20.0, Lengthd::Unit::Em));
  EXPECT_THAT(result.element.computedRy(), LengthIs(30.0, Lengthd::Unit::Ex));

  EXPECT_THAT(result, ElementHas(AllOf(CxEq(0.0, Lengthd::Unit::None),  //
                                       CyEq(0.0, Lengthd::Unit::None),  //
                                       RxIsAuto(),                      //
                                       RyIsAuto())));
}

}  // namespace donner::svg
