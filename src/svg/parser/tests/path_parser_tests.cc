#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/core/tests/path_spline_test_utils.h"
#include "src/svg/parser/path_parser.h"
#include "src/svg/parser/tests/parse_result_test_utils.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::HasSubstr;

namespace donner {

using Command = PathSpline::Command;
using CommandType = PathSpline::CommandType;

TEST(PathParser, Empty) {
  ParseResult<PathSpline> result = PathParser::parse("");
  EXPECT_TRUE(result.hasResult());
  EXPECT_FALSE(result.hasError());

  EXPECT_TRUE(result.result().empty());
}

TEST(PathParser, InvalidInitialCommand) {
  EXPECT_THAT(PathParser::parse("z"), ParseErrorIs(HasSubstr("Unexpected command")));
  EXPECT_THAT(PathParser::parse(" \t\f\r\nz"),
              AllOf(ParseErrorIs(HasSubstr("Unexpected command")), ParseErrorPos(0, 5)));
}

TEST(PathParser, InitialMove) {
  EXPECT_THAT(PathParser::parse("M"), ParseErrorIs(HasSubstr("Failed to parse number")));

  {
    ParseResult<PathSpline> result = PathParser::parse("M 1.2 -5");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.2, -5)));
    EXPECT_THAT(spline.commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
  }

  {
    ParseResult<PathSpline> result = PathParser::parse("M 0 1e2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(0.0, 100.0)));
    EXPECT_THAT(spline.commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
  }
}

TEST(PathParser, LineTo_Implicit) {
  // Uppercase M -> absolute LineTo
  {
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase m -> relative LineTo
  {
    ParseResult<PathSpline> result = PathParser::parse("m 1 1 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 4.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }
}

}  // namespace donner
