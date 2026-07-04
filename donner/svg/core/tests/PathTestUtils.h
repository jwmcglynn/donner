#pragma once

#include <gmock/gmock.h>

#include <vector>

#include "donner/base/Path.h"
#include "gtest/gtest.h"

namespace donner::svg {

/**
 * Matches a Path command by verb.
 *
 * @param expectedVerb Expected Path::Verb value.
 */
MATCHER_P(CommandVerbIs, expectedVerb, "has command verb " + testing::PrintToString(expectedVerb)) {
  *result_listener << "actual command=" << arg;
  return testing::ExplainMatchResult(testing::Eq(expectedVerb), arg.verb, result_listener);
}

/**
 * Matches the verb sequence of a Path commands list.
 *
 * @param verbs Expected Path::Verb values in command order.
 */
template <typename... Verbs>
auto CommandVerbsAre(Verbs... verbs) {
  return testing::ElementsAre(CommandVerbIs(verbs)...);
}

/**
 * Matches the points and commands of a Path.
 *
 * @param pointsMatcher Points array matcher.
 * @param commandsMatcher Commands array matcher.
 */
MATCHER_P2(PointsAndCommandsAre, pointsMatcher, commandsMatcher, "") {
  return testing::ExplainMatchResult(pointsMatcher, arg.points(), result_listener) &&
         testing::ExplainMatchResult(commandsMatcher, arg.commands(), result_listener);
}

/**
 * Matches the points of a from the Path vertices list.
 */
MATCHER_P(VertexPointsAre, pointsMatcher, "") {
  std::vector<Vector2d> vertexPoints;
  for (const auto& vertex : arg) {
    vertexPoints.push_back(vertex.point);
  }

  return testing::ExplainMatchResult(pointsMatcher, vertexPoints, result_listener);
}

/**
 * Matches the points of a from the Path vertices list. Variadic template function that
 * accepts matchers or expected values for each vertex point.
 */
template <typename... Matchers>
auto VertexPointsAre(Matchers... matchers) {
  return VertexPointsAre(testing::ElementsAre(matchers...));
}

}  // namespace donner::svg
