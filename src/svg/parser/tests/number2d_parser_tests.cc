#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/parse_result.h"
#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/svg/parser/number2d_parser.h"

using testing::AllOf;
using testing::ElementsAre;

namespace donner::svg {

namespace {

MATCHER_P2(Number2dIs, x, y, "") {
  return arg.numberX == x && arg.numberY == y;
}

}  // namespace

TEST(Number2dParser, Empty) {
  ParseResult<Number2dParser::Result> result = Number2dParser::Parse("");
  EXPECT_FALSE(result.hasResult());
  EXPECT_THAT(result, ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(Number2dParser, OneNumber) {
  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("0");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(0.0, 0.0));
  }

  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("1");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(1.0, 1.0));
  }

  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("1.2");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(1.2, 1.2));
  }

  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("1e2");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(100.0, 100.0));
  }
}

TEST(Number2dParser, TwoNumbers) {
  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("0 0");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(0.0, 0.0));
  }

  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("1 2");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(1.0, 2.0));
  }

  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("1.2 3.4");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(1.2, 3.4));
  }

  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("1e2 3e4");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(100.0, 30000.0));
  }
}

TEST(Number2dParser, NoSpaces) {
  ParseResult<Number2dParser::Result> result = Number2dParser::Parse("-1-2");
  ASSERT_THAT(result, NoParseError());
  EXPECT_THAT(result.result(), Number2dIs(-1.0, -2.0));
}

TEST(Number2dParser, ExtraCharacters) {
  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("1 2 3");
    ASSERT_THAT(result, NoParseError());
    EXPECT_THAT(result.result(), Number2dIs(1.0, 2.0));
    EXPECT_THAT(result.result().consumedChars, testing::Eq(3));
  }
}

TEST(Number2dParser, ParseErrors) {
  {
    ParseResult<Number2dParser::Result> result = Number2dParser::Parse("1,2");
    EXPECT_THAT(result, ParseErrorIs("Failed to parse number: Unexpected character"));
  }
}

}  // namespace donner::svg
