#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/parser/path_parser.h"
#include "src/svg/parser/tests/parse_result_test_utils.h"

namespace donner {

TEST(PathParser, Empty) {
  ParseResult<PathSpline> result = PathParser::parse("");
  EXPECT_TRUE(result.hasResult());
  EXPECT_FALSE(result.hasError());

  EXPECT_TRUE(result.result().empty());
}

TEST(PathParser, InvalidInitialCommand) {
  ParseResult<PathSpline> result = PathParser::parse("z");
  EXPECT_THAT(result, ParseErrorIs("Not implemented"));
}

}  // namespace donner
