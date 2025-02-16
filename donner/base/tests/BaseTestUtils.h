#pragma once
/// @file

#include <gmock/gmock.h>

namespace donner {

/**
 * Matches the string representation of an object, by calling `testing::PrintToString` and comparing
 * the results with the expected value.
 *
 * Example:
 * ```
 * EXPECT_THAT(myObject, ToStringIs("MyObject(foo)"));
 * ```
 *
 * @param expected Expected string representation.
 */
MATCHER_P(ToStringIs, expected, "") {
  // If the object has an ostream operator, use it to convert to a string. Otherwise callback to
  // `testing::PrintToString`.
  std::string argString;
  if constexpr (requires(std::ostream& os, const decltype(arg)& a) { os << a; }) {
    std::ostringstream ss;
    ss << arg;
    argString = ss.str();
  } else {
    argString = testing::PrintToString(arg);
  }

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
 * Example:
 * ```
 * EXPECT_THAT(Vector2i(1, 2), Vector2Eq(1, 2));
 * ```
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
 * @param xValue X coordinate, note that this is not a matcher.
 * @param yValue Y coordinate, note that this is not a matcher.
 */
MATCHER_P2(Vector2Near, xValue, yValue, "") {
  return testing::ExplainMatchResult(testing::DoubleNear(xValue, 0.01), arg.x, result_listener) &&
         testing::ExplainMatchResult(testing::DoubleNear(yValue, 0.01), arg.y, result_listener);
}

/**
 * Matches if two vectors are equal when both are normalized, within an error of 0.01.
 *
 * Example:
 * ```
 * EXPECT_THAT(Vector2d(1.0, 2.0), NormalizedEq(Vector2d(2.0, 4.0)));
 * ```
 *
 * @param expectedVector Expected value vector, which will be normalized.
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
 * ```
 * EXPECT_THAT(result, TransformEq(Transformd::Scale({2.0, 2.0})));
 * ```
 *
 * @param other Transform object to compare.
 */
MATCHER_P(TransformEq, other, "transform eq " + testing::PrintToString(other)) {
  return testing::ExplainMatchResult(testing::Pointwise(testing::DoubleNear(0.001), other.data),
                                     arg.data, result_listener);
}

/**
 * Matches a transform per-element, with a near-equals comparison using a threshold of 0.0001.
 *
 * Example:
 * ```
 * EXPECT_THAT(result, TransformIs(1.0, 0.0, 0.0, 1.0, 0.0, 0.0));
 * ```
 *
 * @param d0 Corresponds to Transform::data[0]
 * @param d1 Corresponds to Transform::data[1]
 * @param d2 Corresponds to Transform::data[2]
 * @param d3 Corresponds to Transform::data[3]
 * @param d4 Corresponds to Transform::data[4]
 * @param d5 Corresponds to Transform::data[5]
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
 * @param topLeftMatcher Matcher for topLeft field.
 * @param bottomRightMatcher Matcher for bottomRight field.
 */
MATCHER_P2(BoxEq, topLeftMatcher, bottomRightMatcher, "") {
  return testing::ExplainMatchResult(topLeftMatcher, arg.topLeft, result_listener) &&
         testing::ExplainMatchResult(bottomRightMatcher, arg.bottomRight, result_listener);
}

/**
 * Matches a Length.
 *
 * Example:
 * ```
 * EXPECT_THAT(Length(10.0, Lengthd::Unit::Cm), LengthIs(10.0, Lengthd::Unit::Cm));
 * ```
 *
 * @param valueMatcher Matcher for value field.
 * @param unitMatcher Matcher for unit field.
 */
MATCHER_P2(LengthIs, valueMatcher, unitMatcher, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.unit, result_listener);
}

}  // namespace donner
