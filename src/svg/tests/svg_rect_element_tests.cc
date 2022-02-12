#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/svg_rect_element.h"
#include "src/svg/tests/xml_test_utils.h"

using testing::AllOf;

namespace donner::svg {

MATCHER_P2(LengthIs, valueMatcher, unitMatcher, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.unit, result_listener);
}

namespace {

auto XEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x", &SVGRectElement::x, LengthIs(valueMatcher, unitMatcher));
}

auto YEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y", &SVGRectElement::y, LengthIs(valueMatcher, unitMatcher));
}

auto WidthEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("width", &SVGRectElement::width, LengthIs(valueMatcher, unitMatcher));
}

auto HeightEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("height", &SVGRectElement::height, LengthIs(valueMatcher, unitMatcher));
}

auto RxEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("rx", &SVGRectElement::rx, LengthIs(valueMatcher, unitMatcher));
}

auto RyEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("ry", &SVGRectElement::ry, LengthIs(valueMatcher, unitMatcher));
}

MATCHER_P(RectHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

}  // namespace

TEST(SVGRectElementTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGRectElement>(  //
                  "<rect />"),
              RectHas(AllOf(XEq(0.0, Lengthd::Unit::None),      //
                            YEq(0.0, Lengthd::Unit::None),      //
                            WidthEq(0.0, Lengthd::Unit::None),  //
                            HeightEq(0.0, Lengthd::Unit::None))));
}

TEST(SVGRectElementTests, Simple) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGRectElement>(  //
                  R"(<rect x="50" y="40" width="30" height="20" />)"),
              RectHas(AllOf(XEq(50.0, Lengthd::Unit::None),      //
                            YEq(40.0, Lengthd::Unit::None),      //
                            WidthEq(30.0, Lengthd::Unit::None),  //
                            HeightEq(20.0, Lengthd::Unit::None))));
}

TEST(SVGRectElementTests, Units) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGRectElement>(  //
                  R"(<rect x="50px" y="0" width="30em" height="20pt" />)"),
              RectHas(AllOf(XEq(50.0, Lengthd::Unit::Px),      //
                            YEq(0.0, Lengthd::Unit::None),     //
                            WidthEq(30.0, Lengthd::Unit::Em),  //
                            HeightEq(20.0, Lengthd::Unit::Pt))));
}

TEST(SVGRectElementTests, PresentationAttributes) {
  auto result = instantiateSubtreeElementAs<SVGRectElement>(R"(
      <rect />
      <style>
        rect {
          x: 0;
          y: 10px;
          width: 20em;
          height: 30pt;
        }
      </style>
    )");

  EXPECT_THAT(result.element.computedX(), LengthIs(0.0, Lengthd::Unit::None));
  EXPECT_THAT(result.element.computedY(), LengthIs(10.0, Lengthd::Unit::Px));
  EXPECT_THAT(result.element.computedWidth(), LengthIs(20.0, Lengthd::Unit::Em));
  EXPECT_THAT(result.element.computedHeight(), LengthIs(30.0, Lengthd::Unit::Pt));

  EXPECT_THAT(result, RectHas(AllOf(XEq(0.0, Lengthd::Unit::None),      //
                                    YEq(0.0, Lengthd::Unit::None),      //
                                    WidthEq(0.0, Lengthd::Unit::None),  //
                                    HeightEq(0.0, Lengthd::Unit::None))));
}

}  // namespace donner::svg
