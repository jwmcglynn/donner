#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/parser/tests/parse_result_test_utils.h"
#include "src/svg/parser/transform_parser.h"

using testing::DoubleEq;
using testing::ElementsAre;

namespace donner {

/**
 * Matches a transform.
 */
MATCHER_P6(TransformIs, d0, d1, d2, d3, d4, d5, "") {
  return testing::ExplainMatchResult(ElementsAre(DoubleEq(d0), DoubleEq(d1), DoubleEq(d2),
                                                 DoubleEq(d3), DoubleEq(d4), DoubleEq(d5)),
                                     arg.data, result_listener);
}

/**
 * Matches if a transform is identity.
 */
MATCHER(TransformIsIdentity, "") {
  return arg.isIdentity();
}

template <typename T>
void PrintTo(const Transform<T>& t, std::ostream* os) {
  *os << "Transform(" << t.data[0] << " " << t.data[1] << " " << t.data[2] << " " << t.data[3]
      << " " << t.data[4] << " " << t.data[5] << ")";
}

TEST(TransformParser, Empty) {
  EXPECT_THAT(TransformParser::parse(""), ParseResultIs(TransformIsIdentity()));
}

TEST(TransformParser, ParseErrors) {
  EXPECT_THAT(TransformParser::parse("("), ParseErrorIs("Unexpected function ''"));
}

TEST(TransformParser, Matrix) {
  EXPECT_THAT(TransformParser::parse("matrix(1 2 3 4 5 6)"),
              ParseResultIs(TransformIs(1, 2, 3, 4, 5, 6)));

  EXPECT_THAT(TransformParser::parse("matrix ( \t 7 8 9 \r\n 10 11 12 ) "),
              ParseResultIs(TransformIs(7, 8, 9, 10, 11, 12)));

  EXPECT_THAT(TransformParser::parse("matrix(-1-2-3-4-5-6)"),
              ParseResultIs(TransformIs(-1, -2, -3, -4, -5, -6)));

  EXPECT_THAT(TransformParser::parse("matrix(6,5,4 3,2,1)"),
              ParseResultIs(TransformIs(6, 5, 4, 3, 2, 1)));
}

TEST(TransformParser, Matrix_ParseErrors) {
  // No parameters.
  EXPECT_THAT(TransformParser::parse("matrix()"),
              ParseErrorIs("Failed to parse number: Invalid argument"));

  // Too few parameters.
  EXPECT_THAT(TransformParser::parse("matrix(1, 2, 3)"),
              ParseErrorIs("Failed to parse number: Invalid argument"));
  EXPECT_THAT(TransformParser::parse("matrix(1, 2, 3, 4, 5)"),
              ParseErrorIs("Failed to parse number: Invalid argument"));

  // Too many parameters.
  EXPECT_THAT(TransformParser::parse("matrix(1, 2, 3, 4, 5, 6, 7)"), ParseErrorIs("Expected ')'"));

  // Missing parens.
  EXPECT_THAT(TransformParser::parse("matrix"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::parse("matrix 1 2"),
              ParseErrorIs("Expected '(' after function name"));
  EXPECT_THAT(TransformParser::parse("matrix("),
              ParseErrorIs("Failed to parse number: Invalid argument"));
}

}  // namespace donner
