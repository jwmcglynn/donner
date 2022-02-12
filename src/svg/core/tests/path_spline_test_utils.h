#pragma once

#include <gmock/gmock.h>

#include "src/svg/core/path_spline.h"

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

}  // namespace donner::svg
