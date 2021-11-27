#pragma once

#include <gmock/gmock.h>

namespace donner {

MATCHER_P(ToStringIs, expected, "") {
  const std::string argString = testing::PrintToString(arg);
  const std::string expectedString = expected;

  const bool result = argString == expected;
  if (!result) {
    *result_listener << "\nExpected string: " << expected;

    *result_listener << "\nMatching subset: "
                     << std::string(argString.begin(),
                                    std::mismatch(argString.begin(), argString.end(),
                                                  expectedString.begin(), expectedString.end())
                                        .first);
  }

  return result;
}

/**
 * Matches a Vector.
 *
 * @param xMatcher X coordinate matcher.
 * @param yMatcher Y coordinate matcher.
 */
MATCHER_P2(Vector2Eq, xMatcher, yMatcher, "") {
  return testing::ExplainMatchResult(xMatcher, arg.x, result_listener) &&
         testing::ExplainMatchResult(yMatcher, arg.y, result_listener);
}

/**
 * Matches a Vector2 with DoubleNear(0.01).
 *
 * @param xValue X coordinate.
 * @param yValue Y coordinate.
 */
MATCHER_P2(Vector2Near, xValue, yValue, "") {
  return testing::ExplainMatchResult(testing::DoubleNear(xValue, 0.01), arg.x, result_listener) &&
         testing::ExplainMatchResult(testing::DoubleNear(yValue, 0.01), arg.y, result_listener);
}

/**
 * Matches if two vectors are equal when normalized.
 *
 * @param expectedVector Expected value vector.
 */
MATCHER_P(NormalizedEq, expectedVector, "") {
  const auto normalized = arg.normalize();
  const auto expected = expectedVector.normalize();

  return testing::ExplainMatchResult(testing::DoubleNear(expected.x, 0.01), normalized.x,
                                     result_listener) &&
         testing::ExplainMatchResult(testing::DoubleNear(expected.y, 0.01), normalized.y,
                                     result_listener);
}

/**
 * Matches a transform with near-equals comparison.
 *
 * Example:
 * EXPECT_THAT(result, TransformEq(Transformd::Scale({2.0, 2.0})));
 *
 * @param other Transform object to compare.
 */
MATCHER_P(TransformEq, other, "") {
  return testing::ExplainMatchResult(testing::Pointwise(testing::DoubleNear(0.0001), other.data),
                                     arg.data, result_listener);
}

/**
 * Matches a transform per-element.
 *
 * @param d0 Corresponds to Transform::data[0]
 */
MATCHER_P6(TransformIs, d0, d1, d2, d3, d4, d5, "") {
  return testing::ExplainMatchResult(
      testing::ElementsAre(testing::DoubleNear(d0, 0.0001), testing::DoubleNear(d1, 0.0001),
                           testing::DoubleNear(d2, 0.0001), testing::DoubleNear(d3, 0.0001),
                           testing::DoubleNear(d4, 0.0001), testing::DoubleNear(d5, 0.0001)),
      arg.data, result_listener);
}

/**
 * Matches if a transform is identity.
 */
MATCHER(TransformIsIdentity, "") {
  return arg.isIdentity();
}

/**
 * Matches a Box.
 *
 * @param topLeftMatcher Matcher for top_left field.
 * @param bottomRightMatcher Matcher for bottom_right field.
 */
MATCHER_P2(BoxEq, topLeftMatcher, bottomRightMatcher, "") {
  return testing::ExplainMatchResult(topLeftMatcher, arg.top_left, result_listener) &&
         testing::ExplainMatchResult(bottomRightMatcher, arg.bottom_right, result_listener);
}

}  // namespace donner
