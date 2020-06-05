#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/tests/base_test_utils.h"
#include "src/svg/parser/tests/parse_result_test_utils.h"
#include "src/svg/parser/transform_parser.h"

namespace donner {

TEST(TransformParser, Empty) {
  EXPECT_THAT(TransformParser::Parse(""), ParseResultIs(TransformIsIdentity()));
}

TEST(TransformParser, ParseErrors) {
  EXPECT_THAT(TransformParser::Parse("("), ParseErrorIs("Unexpected function ''"));
}

TEST(TransformParser, Matrix) {
  EXPECT_THAT(TransformParser::Parse("matrix(1 2 3 4 5 6)"),
              ParseResultIs(TransformIs(1, 2, 3, 4, 5, 6)));

  EXPECT_THAT(TransformParser::Parse("matrix ( \t 7 8 9 \r\n 10 11 12 ) "),
              ParseResultIs(TransformIs(7, 8, 9, 10, 11, 12)));

  EXPECT_THAT(TransformParser::Parse("matrix(-1-2-3-4-5-6)"),
              ParseResultIs(TransformIs(-1, -2, -3, -4, -5, -6)));

  EXPECT_THAT(TransformParser::Parse("matrix(6,5,4 3,2,1)"),
              ParseResultIs(TransformIs(6, 5, 4, 3, 2, 1)));
}

TEST(TransformParser, Matrix_ParseErrors) {
  // No parameters.
  EXPECT_THAT(TransformParser::Parse("matrix()"),
              ParseErrorIs("Failed to parse number: Invalid argument"));

  // Too few parameters.
  EXPECT_THAT(TransformParser::Parse("matrix(1, 2, 3)"),
              ParseErrorIs("Failed to parse number: Invalid argument"));
  EXPECT_THAT(TransformParser::Parse("matrix(1, 2, 3, 4, 5)"),
              ParseErrorIs("Failed to parse number: Invalid argument"));

  // Too many parameters.
  EXPECT_THAT(TransformParser::Parse("matrix(1, 2, 3, 4, 5, 6, 7)"), ParseErrorIs("Expected ')'"));

  // Missing parens.
  EXPECT_THAT(TransformParser::Parse("matrix"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::Parse("matrix 1 2"),
              ParseErrorIs("Expected '(' after function name"));
  EXPECT_THAT(TransformParser::Parse("matrix("),
              ParseErrorIs("Failed to parse number: Invalid argument"));
}

}  // namespace donner
