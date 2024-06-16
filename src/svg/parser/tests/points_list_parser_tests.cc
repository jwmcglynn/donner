#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/base/tests/base_test_utils.h"
#include "src/svg/parser/points_list_parser.h"

using testing::AllOf;
using testing::ElementsAre;

namespace donner::svg::parser {

using namespace base::parser;  // NOLINT: For tests

TEST(PointsListParser, Empty) {
  ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("");
  EXPECT_TRUE(result.hasResult());
  EXPECT_FALSE(result.hasError());

  EXPECT_TRUE(result.result().empty());
}

TEST(PointsListParser, OnePoint) {
  EXPECT_THAT(PointsListParser::Parse("0 0"), NoParseError());
  EXPECT_THAT(PointsListParser::Parse("0,0"), NoParseError());
  EXPECT_THAT(PointsListParser::Parse("0\n,\t0"), NoParseError());

  {
    ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("1.2 -5");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), ElementsAre(Vector2d(1.2, -5)));
  }

  {
    ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("0 1e2");
    ASSERT_THAT(result, NoParseError());
  }
}

TEST(PointsListParser, NoSpaces) {
  ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("-1-2-3-4-5-6");
  ASSERT_THAT(result, NoParseError());
  EXPECT_THAT(result.result(), ElementsAre(Vector2d(-1, -2), Vector2d(-3, -4), Vector2d(-5, -6)));
}

TEST(PointsListParser, ParseErrors) {
  // Comma before a command is a parse error.
  {
    ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("0,0,");
    EXPECT_THAT(result,
                ParseResultAndError(ElementsAre(Vector2d::Zero()),
                                    ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  {
    ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("1 2,3,,4");
    EXPECT_THAT(result,
                ParseResultAndError(ElementsAre(Vector2d(1, 2)),
                                    ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  {
    ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("1 2,4,5,3e3");
    EXPECT_THAT(result,
                ParseResultAndError(ElementsAre(Vector2d(1, 2), Vector2d(4, 5)),
                                    ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  // Unexpected tokens.
  EXPECT_THAT(PointsListParser::Parse("b"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

}  // namespace donner::svg::parser
