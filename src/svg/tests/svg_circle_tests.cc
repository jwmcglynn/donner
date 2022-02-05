#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/svg_circle_element.h"
#include "src/svg/tests/xml_test_utils.h"

using testing::AllOf;
using testing::Property;

namespace donner {

MATCHER_P2(LengthIs, valueMatcher, unitMatcher, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.unit, result_listener);
}

namespace {

auto CxEq(auto valueMatcher, auto unitMatcher) {
  return Property("cx", &SVGCircleElement::cx, LengthIs(valueMatcher, unitMatcher));
}

auto CyEq(auto valueMatcher, auto unitMatcher) {
  return Property("cy", &SVGCircleElement::cy, LengthIs(valueMatcher, unitMatcher));
}

auto REq(auto valueMatcher, auto unitMatcher) {
  return Property("r", &SVGCircleElement::r, LengthIs(valueMatcher, unitMatcher));
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

}  // namespace donner
