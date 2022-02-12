#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/svg_circle_element.h"
#include "src/svg/tests/xml_test_utils.h"

using testing::AllOf;

namespace donner::svg {

MATCHER_P2(LengthIs, valueMatcher, unitMatcher, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.unit, result_listener);
}

namespace {

auto CxEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("cx", &SVGCircleElement::cx, LengthIs(valueMatcher, unitMatcher));
}

auto CyEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("cy", &SVGCircleElement::cy, LengthIs(valueMatcher, unitMatcher));
}

auto REq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("r", &SVGCircleElement::r, LengthIs(valueMatcher, unitMatcher));
}

MATCHER_P(CircleHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

}  // namespace

TEST(SVGCircleTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGCircleElement>(  //
                  "<circle />"),
              CircleHas(AllOf(CxEq(0.0, Lengthd::Unit::None),  //
                              CyEq(0.0, Lengthd::Unit::None),  //
                              REq(0.0, Lengthd::Unit::None))));
}

TEST(SVGCircleTests, Simple) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGCircleElement>(  //
                  R"(<circle cx="50" cy="50" r="40" />)"),
              CircleHas(AllOf(CxEq(50.0, Lengthd::Unit::None),  //
                              CyEq(50.0, Lengthd::Unit::None),  //
                              REq(40.0, Lengthd::Unit::None))));
}

TEST(SVGCircleTests, Units) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGCircleElement>(  //
                  R"(<circle cx="50px" cy="30em" r="0" />)"),
              CircleHas(AllOf(CxEq(50.0, Lengthd::Unit::Px),  //
                              CyEq(30.0, Lengthd::Unit::Em),  //
                              REq(0.0, Lengthd::Unit::None))));
}

TEST(SVGCircleTests, PresentationAttributes) {
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

  EXPECT_THAT(result.element.computedCx(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(result.element.computedCy(), LengthIs(10.0, Lengthd::Unit::Px));
  EXPECT_THAT(result.element.computedR(), LengthIs(20.0, Lengthd::Unit::Em));

  EXPECT_THAT(result, CircleHas(AllOf(CxEq(0.0, Lengthd::Unit::None),  //
                                      CyEq(0.0, Lengthd::Unit::None),  //
                                      REq(0.0, Lengthd::Unit::None))));
}

}  // namespace donner::svg
