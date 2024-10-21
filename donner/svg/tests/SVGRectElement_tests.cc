#include "donner/svg/SVGRectElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/core/tests/PathSplineTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

namespace donner::svg {

using Command = PathSpline::Command;
using CommandType = PathSpline::CommandType;

namespace {

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

auto RxIsAuto() {
  return testing::Property("rx", &SVGRectElement::rx, Eq(std::nullopt));
}

auto RxEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("rx", &SVGRectElement::rx,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

auto RyIsAuto() {
  return testing::Property("ry", &SVGRectElement::ry, Eq(std::nullopt));
}

auto RyEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("ry", &SVGRectElement::ry,
                           Optional(LengthIs(valueMatcher, unitMatcher)));
}

MATCHER_P(ElementHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

}  // namespace

TEST(SVGRectElementTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGRectElement>(  //
                  "<rect />"),
              ElementHas(AllOf(XEq(0.0, Lengthd::Unit::None),      //
                               YEq(0.0, Lengthd::Unit::None),      //
                               WidthEq(0.0, Lengthd::Unit::None),  //
                               HeightEq(0.0, Lengthd::Unit::None))));
}

TEST(SVGRectElementTests, Simple) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGRectElement>(  //
                  R"(<rect x="50" y="40" width="30" height="20" />)"),
              ElementHas(AllOf(XEq(50.0, Lengthd::Unit::None),       //
                               YEq(40.0, Lengthd::Unit::None),       //
                               WidthEq(30.0, Lengthd::Unit::None),   //
                               HeightEq(20.0, Lengthd::Unit::None),  //
                               RxIsAuto(),                           //
                               RyIsAuto())));
}

TEST(SVGRectElementTests, RoundedCorners) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGRectElement>(  //
                  R"(<rect x="50" y="40" width="30" height="20" rx="5" ry="6" />)"),
              ElementHas(AllOf(RxEq(5.0, Lengthd::Unit::None),  //
                               RyEq(6.0, Lengthd::Unit::None))));
}

TEST(SVGRectElementTests, Units) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGRectElement>(  //
                  R"(<rect x="50px" y="0" width="30em" height="20pt" />)"),
              ElementHas(AllOf(XEq(50.0, Lengthd::Unit::Px),      //
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

  EXPECT_THAT(result, ElementHas(AllOf(XEq(0.0, Lengthd::Unit::None),      //
                                       YEq(0.0, Lengthd::Unit::None),      //
                                       WidthEq(0.0, Lengthd::Unit::None),  //
                                       HeightEq(0.0, Lengthd::Unit::None))));
}

TEST(SVGRectElementTests, Spline) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGRectElement>(  //
          R"(<rect x="50" y="40" width="30" height="20" />)"),
      ComputedSplineIs(PointsAndCommandsAre(
          ElementsAre(Vector2d(50, 40), Vector2d(80, 40), Vector2d(80, 60), Vector2d(50, 60)),  //
          ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                      Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3},
                      Command{CommandType::ClosePath, 0}))));
}

TEST(SVGRectElementTests, SplineRoundedCorners) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGRectElement>(  //
          R"(<rect x="50" y="40" width="30" height="20" rx="4" ry="4"/>)"),
      ComputedSplineIs(PointsAndCommandsAre(
          ElementsAre(Vector2d(54, 40), Vector2d(76, 40), Vector2Near(78.2091, 40),
                      Vector2Near(80, 41.7909), Vector2d(80, 44), Vector2d(80, 56),
                      Vector2Near(80, 58.2091), Vector2Near(78.2091, 60), Vector2d(76, 60),
                      Vector2d(54, 60), Vector2Near(51.7909, 60), Vector2Near(50, 58.2091),
                      Vector2d(50, 56), Vector2d(50, 44), Vector2Near(50, 41.7909),
                      Vector2Near(51.7909, 40), Vector2d(54, 40)),  //
          ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                      Command{CommandType::CurveTo, 2}, Command{CommandType::LineTo, 5},
                      Command{CommandType::CurveTo, 6}, Command{CommandType::LineTo, 9},
                      Command{CommandType::CurveTo, 10}, Command{CommandType::LineTo, 13},
                      Command{CommandType::CurveTo, 14}, Command{CommandType::ClosePath, 0}))));
}

}  // namespace donner::svg
