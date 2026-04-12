#include "donner/svg/SVGPathElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/Path.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/core/tests/PathTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::ElementsAre;
using testing::Optional;

namespace donner::svg {

using Command = Path::Command;
using CommandType = Path::Verb;

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
  EXPECT_EQ(instantiateSubtreeElementAs<SVGPathElement>("<path />")->d(), "");
  EXPECT_DOUBLE_EQ(instantiateSubtreeElementAs<SVGPathElement>("<path />")->computedPathLength(), 0.0);
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

TEST(SVGPathElementTests, DAttributeAndSetters) {
  auto fragment = instantiateSubtreeElementAs<SVGPathElement>(R"(<path d="M 0 0 L 3 4" />)");

  EXPECT_EQ(fragment->d(), "M 0 0 L 3 4");
  EXPECT_DOUBLE_EQ(fragment->computedPathLength(), 5.0);
  EXPECT_EQ(fragment->pathLength(), std::nullopt);

  fragment->setPathLength(42.0);
  EXPECT_THAT(fragment->pathLength(), Optional(42.0));
  fragment->setPathLength(std::nullopt);
  EXPECT_EQ(fragment->pathLength(), std::nullopt);

  fragment->setD("M 1 1 L 2 2");
  EXPECT_EQ(fragment->d(), "M 1 1 L 2 2");
  ASSERT_TRUE(fragment->computedSpline().has_value());

  const Path spline = PathBuilder().moveTo(Vector2d(10, 20)).lineTo(Vector2d(30, 40)).build();
  fragment->setSpline(spline);
  EXPECT_EQ(fragment->d(), "");
  ASSERT_TRUE(fragment->computedSpline().has_value());
  EXPECT_THAT(fragment->computedSpline().value(),
              PointsAndCommandsAre(
                  ElementsAre(Vector2d(10, 20), Vector2d(30, 40)),
                  ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1})));
}

TEST(SVGPathElementTests, DReturnsEmptyWhenPathComponentRemoved) {
  SVGDocument document;
  SVGPathElement path = SVGPathElement::Create(document);
  path.entityHandle().remove<components::PathComponent>();
  EXPECT_EQ(path.d(), "");
}

TEST(SVGPathElementTests, WorldBounds) {
  auto fragment = instantiateSubtreeElementAs<SVGPathElement>(R"(<path d="M 1 2 L 5 8" />)");

  ASSERT_TRUE(fragment->worldBounds().has_value());
  const Box2d bounds = fragment->worldBounds().value();
  EXPECT_EQ(bounds.topLeft, Vector2d(1, 2));
  EXPECT_EQ(bounds.bottomRight, Vector2d(5, 8));
}

}  // namespace donner::svg
