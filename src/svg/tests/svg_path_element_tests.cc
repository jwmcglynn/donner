#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/core/tests/path_spline_test_utils.h"
#include "src/svg/svg_path_element.h"
#include "src/svg/tests/xml_test_utils.h"

using testing::ElementsAre;

namespace donner::svg {

using Command = PathSpline::Command;
using CommandType = PathSpline::CommandType;

namespace {

MATCHER(ComputedSplineIsEmpty, "") {
  return !arg.element.computedSpline().has_value();
}

MATCHER_P(ComputedSplineIs, matchers, "") {
  const auto maybeSpline = arg.element.computedSpline();
  if (maybeSpline) {
    return testing::ExplainMatchResult(matchers, maybeSpline.value(), result_listener);
  } else {
    *result_listener << "spline is empty";
    return false;
  }
}

}  // namespace

TEST(SVGPathElementTests, Defaults) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGPathElement>(  //
                  "<path />"),
              ComputedSplineIsEmpty());
}

TEST(SVGPathElementTests, Simple) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGPathElement>(  //
          R"(<path d="M 0 0 z" />)"),
      ComputedSplineIs(PointsAndCommandsAre(
          ElementsAre(Vector2d::Zero()),  //
          ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::ClosePath, 0}))));
}

TEST(SVGPathElementTests, PresentationAttributes) {
  EXPECT_THAT(instantiateSubtreeElementAs<SVGPathElement>(R"(
      <path />
      <style>
        path {
          d: none;
        }
      </style>
    )"),
              ComputedSplineIsEmpty());

  EXPECT_THAT(instantiateSubtreeElementAs<SVGPathElement>(R"(
      <path />
      <style d="M 0 0 z">
        path {
          d: none;
        }
      </style>
    )"),
              ComputedSplineIsEmpty())
      << "CSS should override presentation attributes.";

  EXPECT_THAT(instantiateSubtreeElementAs<SVGPathElement>(R"(
      <path />
      <style>
        path {
          d: "M 1 1 L 2 3";
        }
      </style>
    )"),
              ComputedSplineIs(PointsAndCommandsAre(
                  ElementsAre(Vector2d(1, 1), Vector2d(2, 3)),  //
                  ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}))));
}

}  // namespace donner::svg
