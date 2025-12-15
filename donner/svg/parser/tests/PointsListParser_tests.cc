#include "donner/svg/parser/PointsListParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"

using testing::AllOf;
using testing::ElementsAre;

namespace donner::svg::parser {

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
    EXPECT_FALSE(result.hasResult());
    EXPECT_THAT(result, ParseErrorIs("Failed to parse number: Unexpected end of string"));
  }

  {
    ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("1 2,3,,4");
    EXPECT_FALSE(result.hasResult());
    EXPECT_THAT(result, ParseErrorIs("Failed to parse number: Unexpected character"));
  }

  {
    ParseResult<std::vector<Vector2d>> result = PointsListParser::Parse("1 2,4,5,3e3");
    EXPECT_FALSE(result.hasResult());
    EXPECT_THAT(result, ParseErrorIs("Failed to parse number: Unexpected end of string"));
  }

  // Unexpected tokens.
  EXPECT_THAT(PointsListParser::Parse("b"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

}  // namespace donner::svg::parser
