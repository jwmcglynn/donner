#pragma once

#include <gmock/gmock.h>

#include <ostream>

#include "src/svg/core/path_spline.h"

namespace donner {

bool operator==(const PathSpline::Command& lhs, const PathSpline::Command& rhs);

std::ostream& operator<<(std::ostream& os, PathSpline::CommandType type);
std::ostream& operator<<(std::ostream& os, const PathSpline::Command& command);

void PrintTo(const PathSpline& spline, std::ostream* os);

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

/** Matches a Vector.
 * 
 * @param xMatcher X coordinate matcher.
 * @param yMatcher Y coordinate matcher.
 */
MATCHER_P2(Vector2Eq, xMatcher, yMatcher, "") {
  return testing::ExplainMatchResult(xMatcher, arg.x, result_listener) &&
         testing::ExplainMatchResult(yMatcher, arg.y, result_listener);
}

/** Matches a Vector2 with DoubleNear(0.01).
 * 
 * @param xValue X coordinate.
 * @param yValue Y coordinate.
 */
MATCHER_P2(Vector2Near, xValue, yValue, "") {
  return testing::ExplainMatchResult(testing::DoubleNear(xValue, 0.01), arg.x, result_listener) &&
         testing::ExplainMatchResult(testing::DoubleNear(yValue, 0.01), arg.y, result_listener);
}

}  // namespace donner
