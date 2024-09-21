#pragma once

#include <gmock/gmock.h>

#include "donner/svg/core/PathSpline.h"
#include "gtest/gtest.h"

namespace donner::svg {

/**
 * Matches the points and commands of a PathSpline.
 *
 * @param pointsMatcher Points array matcher.
 * @param commandsMatcher Commands array matcher.
 */
MATCHER_P2(PointsAndCommandsAre, pointsMatcher, commandsMatcher, "") {
  return testing::ExplainMatchResult(pointsMatcher, arg.points(), result_listener) &&
         testing::ExplainMatchResult(commandsMatcher, arg.commands(), result_listener);
}

/**
 * Matches the points of a from the PathSpline vertices list.
 */
MATCHER_P(VertexPointsAre, pointsMatcher, "") {
  std::vector<Vector2d> vertexPoints;
  for (const auto& vertex : arg) {
    vertexPoints.push_back(vertex.point);
  }

  return testing::ExplainMatchResult(pointsMatcher, vertexPoints, result_listener);
}

/**
 * Matches the points of a from the PathSpline vertices list. Variadic template function that
 * accepts matchers or expected values for each vertex point.
 */
template <typename... Matchers>
auto VertexPointsAre(Matchers... matchers) {
  return VertexPointsAre(testing::ElementsAre(matchers...));
}

}  // namespace donner::svg
