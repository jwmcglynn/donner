#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/base/tests/base_test_utils.h"
#include "src/svg/core/tests/path_spline_test_utils.h"
#include "src/svg/parser/path_parser.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::HasSubstr;

namespace donner::svg {

using Command = PathSpline::Command;
using CommandType = PathSpline::CommandType;

TEST(PathParser, Empty) {
  ParseResult<PathSpline> result = PathParser::Parse("");
  EXPECT_TRUE(result.hasResult());
  EXPECT_FALSE(result.hasError());

  EXPECT_TRUE(result.result().empty());
}

TEST(PathParser, InvalidInitialCommand) {
  EXPECT_THAT(PathParser::Parse("z"), ParseErrorIs(HasSubstr("Unexpected command")));
  EXPECT_THAT(PathParser::Parse(" \t\f\r\nz"),
              AllOf(ParseErrorIs(HasSubstr("Unexpected command")), ParseErrorPos(0, 5)));
}

TEST(PathParser, InitialMoveTo) {
  EXPECT_THAT(PathParser::Parse("M"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M 0"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0 0"), NoParseError());
  EXPECT_THAT(PathParser::Parse("M0,0"), NoParseError());
  EXPECT_THAT(PathParser::Parse("M0\n,\t0"), NoParseError());

  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1.2 -5");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.2, -5)));
    EXPECT_THAT(spline.commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M 0 1e2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(0.0, 100.0)));
    EXPECT_THAT(spline.commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
  }
}

TEST(PathParser, MoveTo) {
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 0 0 1 1 M 2 2 0 0");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0),
                                             Vector2d(2.0, 2.0), Vector2d::Zero()));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, ParseErrors) {
  // Comma before a command is a parse error.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M0,0,Z");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d::Zero()),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Unexpected ',' before command")));
  }

  // Unexpected tokens.
  EXPECT_THAT(PathParser::Parse("b"), ParseErrorIs("Unexpected token 'b' in path data"));

  // Until a valid command is received, the next argument is interpreted as a number.
  EXPECT_THAT(PathParser::Parse("M 0 0 b"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(PathParser, ClosePath) {
  // Use z and Z interchangeably, they should be equivalent.

  // Immediate ClosePath.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 0 0 z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero()));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::ClosePath, 0}));
  }

  // ClosePath without any additional commands should have the last MoveTo stripped.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 0 0 1 1 Z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::ClosePath, 0}));
  }

  // ClosePath followed by a line, contains a MoveTo then a LineTo.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 0 0 1 1 z L -1 -1");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0), Vector2d(-1.0, -1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::ClosePath, 0}, Command{CommandType::MoveTo, 0},
                            Command{CommandType::LineTo, 2}));
  }

  // ClosePath with the MoveTo overridden.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 0 0 1 1 Z M -2 -2 -1 -1");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0),
                                             Vector2d(-2.0, -2.0), Vector2d(-1.0, -1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::ClosePath, 0}, Command{CommandType::MoveTo, 2},
                            Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, ClosePath_ParseErrors) {
  // Comma at end is a parse error.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M0,0Z,");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d::Zero()),
                                                 ElementsAre(Command{CommandType::MoveTo, 0},
                                                             Command{CommandType::ClosePath, 0})),
                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  // No numbers at end, there is no implicit command after.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M0,0Z1");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d::Zero()),
                                                 ElementsAre(Command{CommandType::MoveTo, 0},
                                                             Command{CommandType::ClosePath, 0})),
                            ParseErrorIs("Expected command")));
  }
}

TEST(PathParser, LineTo) {
  // Uppercase L -> absolute LineTo
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 L 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase l -> relative LineTo
  {
    ParseResult<PathSpline> result = PathParser::Parse("m 1 1 l 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 4.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Chain without additional letters.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 0 0 L 1 0 0 1");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d::Zero(), Vector2d(1.0, 0.0), Vector2d(0.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}));
  }

  // Chain with commas.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M0,0L1,0,0,1");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d::Zero(), Vector2d(1.0, 0.0), Vector2d(0.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}));
  }

  // Chain switching relative/absolute
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 0 0 L 1 0 l 1 1 L 0 0");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 0.0),
                                             Vector2d(2.0, 1.0), Vector2d::Zero()));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, LineTo_Implicit) {
  EXPECT_THAT(PathParser::Parse("M0,0 1"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 1"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 1,"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 1, "), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 1,1"), NoParseError());

  // Uppercase M -> absolute LineTo
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase m -> relative LineTo
  {
    ParseResult<PathSpline> result = PathParser::Parse("m 1 1 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 4.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }
}

TEST(PathParser, LineTo_PartialParse) {
  {
    ParseResult<PathSpline> result = PathParser::Parse("M1,1 2,3,");

    EXPECT_THAT(result, ParseResultAndError(PointsAndCommandsAre(
                                                ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)),
                                                ElementsAre(Command{CommandType::MoveTo, 0},
                                                            Command{CommandType::LineTo, 1})),
                                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M1,1 2,3, 4,");

    EXPECT_THAT(result, ParseResultAndError(PointsAndCommandsAre(
                                                ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)),
                                                ElementsAre(Command{CommandType::MoveTo, 0},
                                                            Command{CommandType::LineTo, 1})),
                                            ParseErrorIs(HasSubstr("Failed to parse number"))));
  }
}

TEST(PathParser, HorizontalLineTo) {
  // Uppercase H -> absolute HorizontalLineTo
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 H 2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase h -> relative HorizontalLineTo
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 h 2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Chain between multiple types.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 h 1 h -6 H 0 H -2 h -1");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0), Vector2d(-4.0, 1.0),
                            Vector2d(0.0, 1.0), Vector2d(-2.0, 1.0), Vector2d(-3.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3},
                            Command{CommandType::LineTo, 4}, Command{CommandType::LineTo, 5}));
  }

  // Chain without additional letters.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 h 1 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0),
                                             Vector2d(4.0, 1.0), Vector2d(7.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }

  // Chain with commas.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M1,1h1,2,3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0),
                                             Vector2d(4.0, 1.0), Vector2d(7.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, HorizontalLineTo_ParseError) {
  {
    ParseResult<PathSpline> result = PathParser::Parse("M1,1 h1,");

    EXPECT_THAT(result, ParseResultAndError(PointsAndCommandsAre(
                                                ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0)),
                                                ElementsAre(Command{CommandType::MoveTo, 0},
                                                            Command{CommandType::LineTo, 1})),
                                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M1 1 h");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M1 1 h,");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }
}

TEST(PathParser, VerticalLineTo) {
  // Uppercase V -> absolute VerticalLineTo
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 V 2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 2.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase v -> relative VerticalLineTo
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 v 2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M1 1 v");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M1 1 v,");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  // Chain between multiple types.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 v 1 v -6 V 0 V -2 v -1");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 2.0), Vector2d(1.0, -4.0),
                            Vector2d(1.0, 0.0), Vector2d(1.0, -2.0), Vector2d(1.0, -3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3},
                            Command{CommandType::LineTo, 4}, Command{CommandType::LineTo, 5}));
  }

  // Chain without additional letters.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M 1 1 v 1 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 2.0),
                                             Vector2d(1.0, 4.0), Vector2d(1.0, 7.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }

  // Chain with commas.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M1,1v1,2,3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 2.0),
                                             Vector2d(1.0, 4.0), Vector2d(1.0, 7.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, CurveTo) {
  {
    ParseResult<PathSpline> result =
        PathParser::Parse("M100,200 C100,100 250,100 250,200 S400,300 400,200");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(100.0, 200.0), Vector2d(100.0, 100.0),
                                             Vector2d(250.0, 100.0), Vector2d(250.0, 200.0),
                                             /* auto control point */ Vector2d(250.0, 300.0),
                                             Vector2d(400.0, 300.0), Vector2d(400.0, 200.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                            Command{CommandType::CurveTo, 4}));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M100,200 C100");
    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(100.0, 200.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M100,200 S100");
    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(100.0, 200.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }
}

TEST(PathParser, QuadCurveTo) {
  {
    ParseResult<PathSpline> result = PathParser::Parse("M200,300 Q400,50 600,300 T1000,300");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d(200.0, 300.0), Vector2Near(333.333, 133.333),
                            Vector2Near(466.667, 133.333), Vector2d(600.0, 300.0),
                            Vector2Near(733.333, 466.667), Vector2Near(866.667, 466.667),
                            Vector2d(1000.0, 300.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                            Command{CommandType::CurveTo, 4}));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M200,300 Q400,50 600,");
    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(200.0, 300.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  {
    ParseResult<PathSpline> result = PathParser::Parse("M200,300 T400");
    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(200.0, 300.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }
}

TEST(PathParser, EllipticalArc) {
  {
    /* Confirmed with:
      <path d="M300,200 h-150 a150,150 0 1,0 150,-150 z" />
      <path transform="translate(350 0)"
            d="M300,200 h-150
              C150,282 217,350 300,350
              C382,350 450,282 450,200
              C450,117 382,50 300,50 z" />
    */

    ParseResult<PathSpline> result = PathParser::Parse("M300,200 h-150 a150,150 0 1,0 150,-150 z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(
        spline.points(),
        ElementsAre(Vector2Near(300, 200), Vector2Near(150, 200), Vector2Near(150, 282.843),
                    Vector2Near(217.157, 350), Vector2Near(300, 350), Vector2Near(382.843, 350),
                    Vector2Near(450, 282.843), Vector2Near(450, 200), Vector2Near(450, 117.157),
                    Vector2Near(382.843, 50), Vector2Near(300, 50)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::CurveTo, 2}, Command{CommandType::CurveTo, 5},
                            Command{CommandType::CurveTo, 8}, Command{CommandType::ClosePath, 0}));
  }

  {
    /* Confirmed with:
      <path d="M275,175 v-150 A150,150 0 0,0 125,175 z" />
      <path transform="translate(350 0)"
            d="M275,175 v-150 C192,25 125,92 125,175 z" />
    */

    ParseResult<PathSpline> result = PathParser::Parse("M275,175 v-150 A150,150 0 0,0 125,175 z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(275, 175), Vector2Near(275, 25), Vector2Near(192.157, 25),
                            Vector2Near(125, 92.1573), Vector2Near(125, 175)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::CurveTo, 2}, Command{CommandType::ClosePath, 0}));
  }
}

TEST(PathParser, EllipticalArt_OutOfRangeRadii) {
  // Per https://www.w3.org/TR/SVG/implnote.html#ArcCorrectionOutOfRangeRadii, out-of-range radii
  // should be corrected.

  // Zero radii -> treat as straight line.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M275,175 v-150 A150,0 0 0,0 125,175 z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(275, 175), Vector2Near(275, 25), Vector2Near(125, 175)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::ClosePath, 0}));
  }

  // Negative radii -> take absolute value.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M275,175 v-150 A-150,150 0 0,0 125,175 z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(275, 175), Vector2Near(275, 25), Vector2Near(192.157, 25),
                            Vector2Near(125, 92.1573), Vector2Near(125, 175)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::CurveTo, 2}, Command{CommandType::ClosePath, 0}));
  }

  // Radii too small -> scale them up. Note that this produces a larger arc per the SVG algorithm
  // than the original 150,150 radius, since it minimizes the radius the solution is closer to 2/3
  // of a circle.
  {
    ParseResult<PathSpline> result = PathParser::Parse("M275,175 v-150 A50,50 0 0,0 125,175 z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(275, 175), Vector2Near(275, 25),
                            Vector2Near(233.579, -16.4214), Vector2Near(166.421, -16.4214),
                            Vector2Near(125, 25), Vector2Near(83.5786, 66.4214),
                            Vector2Near(83.5786, 133.579), Vector2Near(125, 175)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::CurveTo, 2}, Command{CommandType::CurveTo, 5},
                            Command{CommandType::ClosePath, 0}));
  }
}

TEST(PathParser, EllipticalArc_Parsing) {
  // Missing rotation.
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150"),
              ParseErrorIs(HasSubstr("Failed to parse number")));

  // Missing flag.
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0"),
              ParseErrorIs("Unexpected end of string when parsing flag"));
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0,"),
              ParseErrorIs("Unexpected end of string when parsing flag"));

  // Invalid flag.
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0 a"),
              ParseErrorIs(HasSubstr("Unexpected character when parsing flag")));
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0 2"),
              ParseErrorIs(HasSubstr("Unexpected character when parsing flag")));
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0 1 a"),
              ParseErrorIs(HasSubstr("Unexpected character when parsing flag")));

  // Missing end point.
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0 0,0"),
              ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0 0,0 150"),
              ParseErrorIs(HasSubstr("Failed to parse number")));

  // No whitespace.
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150,0,0,0,150,150"), NoParseError());
}

TEST(PathParser, NoWhitespace) {
  EXPECT_THAT(PathParser::Parse("M-5-5"),
              ParseResultIs(PointsAndCommandsAre(ElementsAre(Vector2d(-5.0, -5.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0}))));

  EXPECT_THAT(PathParser::Parse("M10-20A5.5.3-4 110-.1"),
              ParseResultIs(PointsAndCommandsAre(
                  ElementsAre(Vector2d(10.0, -20.0), Vector2Near(28.2462, -40.6282),
                              Vector2Near(40.7991, -52.8959), Vector2Near(38.0377, -47.4006),
                              Vector2Near(35.2763, -41.9054), Vector2Near(18.2462, -20.7282),
                              Vector2Near(0, -0.1)),
                  ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                              Command{CommandType::CurveTo, 4}))));

  EXPECT_THAT(
      PathParser::Parse("M10 20V30H40V50H60Z"),
      ParseResultIs(PointsAndCommandsAre(
          ElementsAre(Vector2d(10, 20), Vector2d(10, 30), Vector2d(40, 30), Vector2d(40, 50),
                      Vector2d(60, 50)),
          ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                      Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3},
                      Command{CommandType::LineTo, 4}, Command{CommandType::ClosePath, 0}))));
}

}  // namespace donner::svg
