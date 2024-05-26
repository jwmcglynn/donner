#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/svg_filter_element.h"
#include "src/svg/tests/xml_test_utils.h"

using testing::AllOf;

namespace donner::svg {

MATCHER_P2(LengthIs, valueMatcher, unitMatcher, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.unit, result_listener);
}

namespace {

auto XEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x", &SVGFilterElement::x, LengthIs(valueMatcher, unitMatcher));
}

auto YEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y", &SVGFilterElement::y, LengthIs(valueMatcher, unitMatcher));
}

auto WidthEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("width", &SVGFilterElement::width, LengthIs(valueMatcher, unitMatcher));
}

auto HeightEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("height", &SVGFilterElement::height,
                           LengthIs(valueMatcher, unitMatcher));
}

MATCHER_P(FilterHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

}  // namespace

TEST(SVGFilterElementTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGFilterElement>(  //
                  "<filter />"),
              FilterHas(AllOf(XEq(-10.0, Lengthd::Unit::Percent),      //
                              YEq(-10.0, Lengthd::Unit::Percent),      //
                              WidthEq(120.0, Lengthd::Unit::Percent),  //
                              HeightEq(120.0, Lengthd::Unit::Percent))));
}

}  // namespace donner::svg
