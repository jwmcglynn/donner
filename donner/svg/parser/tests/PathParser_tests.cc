#include "donner/svg/parser/PathParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/core/tests/PathTestUtils.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::HasSubstr;

namespace donner::svg::parser {

using Command = Path::Command;
using CommandType = Path::Verb;

TEST(PathParser, Empty) {
  ParseResult<Path> result = PathParser::Parse("");
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
    ParseResult<Path> result = PathParser::Parse("M 1.2 -5");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.2, -5)));
    EXPECT_THAT(spline.commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M 0 1e2");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(0.0, 100.0)));
    EXPECT_THAT(spline.commands(), ElementsAre(Command{CommandType::MoveTo, 0}));
  }
}

TEST(PathParser, MoveTo) {
  {
    ParseResult<Path> result = PathParser::Parse("M 0 0 1 1 M 2 2 0 0");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
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
    ParseResult<Path> result = PathParser::Parse("M0,0,Z");

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
    ParseResult<Path> result = PathParser::Parse("M 0 0 z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero()));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::ClosePath, 0}));
  }

  // ClosePath without any additional commands should have the last MoveTo stripped.
  {
    ParseResult<Path> result = PathParser::Parse("M 0 0 1 1 Z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::ClosePath, 0}));
  }

  // ClosePath followed by a line, contains a MoveTo then a LineTo.
  {
    ParseResult<Path> result = PathParser::Parse("M 0 0 1 1 z L -1 -1");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    // After closePath, ensureMoveTo() adds a new point at the moveTo position (0,0).
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0), Vector2d::Zero(),
                            Vector2d(-1.0, -1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::ClosePath, 0}, Command{CommandType::MoveTo, 2},
                            Command{CommandType::LineTo, 3}));
  }

  // ClosePath with the MoveTo overridden.
  {
    ParseResult<Path> result = PathParser::Parse("M 0 0 1 1 Z M -2 -2 -1 -1");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0),
                                             Vector2d(-2.0, -2.0), Vector2d(-1.0, -1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::ClosePath, 0}, Command{CommandType::MoveTo, 2},
                            Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, ConsecutiveClosePath) {
  // Multiple consecutive z commands should not crash (regression test for fuzzer crash).
  {
    ParseResult<Path> result = PathParser::Parse("M 0 0 z z z z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    // Consecutive closePaths after the first are no-ops (no open subpath).
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero()));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::ClosePath, 0}));
  }

  // Consecutive z after a line.
  {
    ParseResult<Path> result = PathParser::Parse("M 1 2 L 3 4 z z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    // Second z is a no-op (subpath already closed).
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1, 2), Vector2d(3, 4)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::ClosePath, 0}));
  }
}

TEST(PathParser, ClosePathParseErrors) {
  // Comma at end is a parse error.
  {
    ParseResult<Path> result = PathParser::Parse("M0,0Z,");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d::Zero()),
                                                 ElementsAre(Command{CommandType::MoveTo, 0},
                                                             Command{CommandType::ClosePath, 0})),
                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  // No numbers at end, there is no implicit command after.
  {
    ParseResult<Path> result = PathParser::Parse("M0,0Z1");

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
    ParseResult<Path> result = PathParser::Parse("M 1 1 L 2 3");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase l -> relative LineTo
  {
    ParseResult<Path> result = PathParser::Parse("m 1 1 l 2 3");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 4.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Chain without additional letters.
  {
    ParseResult<Path> result = PathParser::Parse("M 0 0 L 1 0 0 1");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d::Zero(), Vector2d(1.0, 0.0), Vector2d(0.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}));
  }

  // Chain with commas.
  {
    ParseResult<Path> result = PathParser::Parse("M0,0L1,0,0,1");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d::Zero(), Vector2d(1.0, 0.0), Vector2d(0.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}));
  }

  // Chain switching relative/absolute
  {
    ParseResult<Path> result = PathParser::Parse("M 0 0 L 1 0 l 1 1 L 0 0");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 0.0),
                                             Vector2d(2.0, 1.0), Vector2d::Zero()));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, LineToImplicit) {
  EXPECT_THAT(PathParser::Parse("M0,0 1"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 1"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 1,"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 1, "), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::Parse("M0,0 1,1"), NoParseError());

  // Uppercase M -> absolute LineTo
  {
    ParseResult<Path> result = PathParser::Parse("M 1 1 2 3");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase m -> relative LineTo
  {
    ParseResult<Path> result = PathParser::Parse("m 1 1 2 3");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 4.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }
}

TEST(PathParser, LineToPartialParse) {
  {
    ParseResult<Path> result = PathParser::Parse("M1,1 2,3,");

    EXPECT_THAT(result, ParseResultAndError(PointsAndCommandsAre(
                                                ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)),
                                                ElementsAre(Command{CommandType::MoveTo, 0},
                                                            Command{CommandType::LineTo, 1})),
                                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M1,1 2,3, 4,");

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
    ParseResult<Path> result = PathParser::Parse("M 1 1 H 2");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase h -> relative HorizontalLineTo
  {
    ParseResult<Path> result = PathParser::Parse("M 1 1 h 2");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Chain between multiple types.
  {
    ParseResult<Path> result = PathParser::Parse("M 1 1 h 1 h -6 H 0 H -2 h -1");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
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
    ParseResult<Path> result = PathParser::Parse("M 1 1 h 1 2 3");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0),
                                             Vector2d(4.0, 1.0), Vector2d(7.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }

  // Chain with commas.
  {
    ParseResult<Path> result = PathParser::Parse("M1,1h1,2,3");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0),
                                             Vector2d(4.0, 1.0), Vector2d(7.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, HorizontalLineToParseError) {
  {
    ParseResult<Path> result = PathParser::Parse("M1,1 h1,");

    EXPECT_THAT(result, ParseResultAndError(PointsAndCommandsAre(
                                                ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0)),
                                                ElementsAre(Command{CommandType::MoveTo, 0},
                                                            Command{CommandType::LineTo, 1})),
                                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M1 1 h");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected end of string")));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M1 1 h,");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }
}

TEST(PathParser, VerticalLineTo) {
  // Uppercase V -> absolute VerticalLineTo
  {
    ParseResult<Path> result = PathParser::Parse("M 1 1 V 2");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 2.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase v -> relative VerticalLineTo
  {
    ParseResult<Path> result = PathParser::Parse("M 1 1 v 2");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M1 1 v");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected end of string")));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M1 1 v,");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected character")));
  }

  // Chain between multiple types.
  {
    ParseResult<Path> result = PathParser::Parse("M 1 1 v 1 v -6 V 0 V -2 v -1");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
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
    ParseResult<Path> result = PathParser::Parse("M 1 1 v 1 2 3");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 2.0),
                                             Vector2d(1.0, 4.0), Vector2d(1.0, 7.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }

  // Chain with commas.
  {
    ParseResult<Path> result = PathParser::Parse("M1,1v1,2,3");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 2.0),
                                             Vector2d(1.0, 4.0), Vector2d(1.0, 7.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, CurveTo) {
  {
    ParseResult<Path> result =
        PathParser::Parse("M100,200 C100,100 250,100 250,200 S400,300 400,200");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(100.0, 200.0), Vector2d(100.0, 100.0),
                                             Vector2d(250.0, 100.0), Vector2d(250.0, 200.0),
                                             /* auto control point */ Vector2d(250.0, 300.0),
                                             Vector2d(400.0, 300.0), Vector2d(400.0, 200.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::CurveTo, 1},
                            Command{CommandType::CurveTo, 4}));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M100,200 C100");
    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(100.0, 200.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected end of string")));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M100,200 S100");
    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(100.0, 200.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected end of string")));
  }
}

TEST(PathParser, QuadCurveTo) {
  {
    ParseResult<Path> result = PathParser::Parse("M200,300 Q400,50 600,300 T1000,300");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    // QuadCurveTo now emits QuadTo directly (no degree elevation to cubic).
    // Q400,50 600,300 -> QuadTo with control=(400,50), end=(600,300)
    // T1000,300 -> reflected control=(800,550), end=(1000,300)
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2d(200.0, 300.0), Vector2d(400.0, 50.0),
                            Vector2d(600.0, 300.0), Vector2d(800.0, 550.0),
                            Vector2d(1000.0, 300.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::QuadTo, 1},
                            Command{CommandType::QuadTo, 3}));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M200,300 Q400,50 600,");
    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(200.0, 300.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected end of string")));
  }

  {
    ParseResult<Path> result = PathParser::Parse("M200,300 T400");
    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(200.0, 300.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Unexpected end of string")));
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

    ParseResult<Path> result = PathParser::Parse("M300,200 h-150 a150,150 0 1,0 150,-150 z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    // Proper SVG arc decomposition: 270-degree arc produces 3 cubic Bezier segments.
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(300, 200), Vector2Near(150, 200),
                            Vector2Near(150, 282.84), Vector2Near(217.16, 350),
                            Vector2Near(300, 350), Vector2Near(382.84, 350),
                            Vector2Near(450, 282.84), Vector2Near(450, 200),
                            Vector2Near(450, 117.16), Vector2Near(382.84, 50),
                            Vector2Near(300, 50)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::CurveTo, 2}, Command{CommandType::CurveTo, 5},
                            Command{CommandType::CurveTo, 8},
                            Command{CommandType::ClosePath, 0}));
  }

  {
    /* Confirmed with:
      <path d="M275,175 v-150 A150,150 0 0,0 125,175 z" />
      <path transform="translate(350 0)"
            d="M275,175 v-150 C192,25 125,92 125,175 z" />
    */

    ParseResult<Path> result = PathParser::Parse("M275,175 v-150 A150,150 0 0,0 125,175 z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    // Proper SVG arc decomposition: 90-degree arc produces 1 cubic Bezier segment.
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(275, 175), Vector2Near(275, 25), Vector2Near(192.16, 25),
                            Vector2Near(125, 92.16), Vector2Near(125, 175)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::CurveTo, 2}, Command{CommandType::ClosePath, 0}));
  }
}

TEST(PathParser, EllipticalArtOutOfRangeRadii) {
  // Per https://www.w3.org/TR/SVG/implnote.html#ArcCorrectionOutOfRangeRadii, out-of-range radii
  // should be corrected.

  // Zero radii -> degenerates to a straight line per SVG spec.
  {
    ParseResult<Path> result = PathParser::Parse("M275,175 v-150 A150,0 0 0,0 125,175 z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(275, 175), Vector2Near(275, 25), Vector2Near(125, 175)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::LineTo, 2}, Command{CommandType::ClosePath, 0}));
  }

  // Negative radii -> take absolute value. Proper SVG arc decomposition.
  {
    ParseResult<Path> result = PathParser::Parse("M275,175 v-150 A-150,150 0 0,0 125,175 z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(275, 175), Vector2Near(275, 25), Vector2Near(192.16, 25),
                            Vector2Near(125, 92.16), Vector2Near(125, 175)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::CurveTo, 2}, Command{CommandType::ClosePath, 0}));
  }

  // Radii too small -> scale them up. Proper SVG arc decomposition produces 2 cubic segments.
  {
    ParseResult<Path> result = PathParser::Parse("M275,175 v-150 A50,50 0 0,0 125,175 z");
    ASSERT_THAT(result, NoParseError());

    Path spline = result.result();
    EXPECT_THAT(spline.points(),
                ElementsAre(Vector2Near(275, 175), Vector2Near(275, 25),
                            Vector2Near(233.58, -16.42), Vector2Near(166.42, -16.42),
                            Vector2Near(125, 25), Vector2Near(83.58, 66.42),
                            Vector2Near(83.58, 133.58), Vector2Near(125, 175)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::CurveTo, 2}, Command{CommandType::CurveTo, 5},
                            Command{CommandType::ClosePath, 0}));
  }
}

TEST(PathParser, EllipticalArcParsing) {
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

  // Proper SVG arc decomposition produces two cubic Bezier segments.
  EXPECT_THAT(PathParser::Parse("M10-20A5.5.3-4 110-.1"),
              ParseResultIs(PointsAndCommandsAre(
                  ElementsAre(Vector2d(10.0, -20.0), Vector2Near(106.745, -26.59),
                              Vector2Near(182.933, -27.48), Vector2Near(180.172, -21.99),
                              Vector2Near(177.41, -16.49), Vector2Near(96.74, -6.69),
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

// ---------------------------------------------------------------------------
// Range-accuracy tests: verify that error SourceRanges cover the right span.
// ---------------------------------------------------------------------------

TEST(PathParser, RangeInvalidInitialCommand) {
  // "z" at offset 0 => range covers the single character [0,1).
  // PathParser reads the character and calls rangeFrom(offset), which is [0, consumed).
  EXPECT_THAT(PathParser::Parse("z"), ParseErrorRange(0, 1));
  // With leading whitespace, "z" is at offset 5.
  EXPECT_THAT(PathParser::Parse(" \t\f\r\nz"), ParseErrorRange(5, 6));
}

TEST(PathParser, RangeUnexpectedToken) {
  // "M0 0 !" => "!" is at offset 5 => [5,6).
  EXPECT_THAT(PathParser::Parse("M0 0 !"), ParseErrorRange(5, 6));
}

TEST(PathParser, RangeInvalidFlag) {
  // "M0,0 a150,150 0 a" => the flag 'a' is at offset 16 => [16,17).
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0 a"), ParseErrorRange(16, 17));
  // "M0,0 a150,150 0 2" => '2' at offset 16 => [16,17).
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0 2"), ParseErrorRange(16, 17));
}

TEST(PathParser, RangeEndOfStringFlag) {
  // "M0,0 a150,150 0" => end of string when expecting flag.
  EXPECT_THAT(PathParser::Parse("M0,0 a150,150 0"), ParseErrorEndOfString());
}

TEST(PathParser, RangeTrailingComma) {
  // "M1,1 h1," => comma at offset 7 => [7,8).
  EXPECT_THAT(PathParser::Parse("M1,1 h1,"), ParseErrorRange(7, 8));
}

// =============================================================================
// toSVGPathData round-trip: parse → serialize → re-parse must yield same Path
// =============================================================================

namespace {

/// Parse a path string, serialize it back, then re-parse and compare.
/// Both the original and the round-tripped result must have no error, and the
/// re-parsed points/commands must match the original exactly.
void ExpectRoundTrip(std::string_view d) {
  ParseResult<Path> first = PathParser::Parse(d);
  ASSERT_THAT(first, NoParseError()) << "First parse of: " << d;

  const Path& path = first.result();
  RcString serialized = path.toSVGPathData();

  ParseResult<Path> second = PathParser::Parse(serialized);
  ASSERT_THAT(second, NoParseError())
      << "Re-parse failed for serialized: " << serialized;

  const Path& roundTripped = second.result();
  ASSERT_EQ(roundTripped.verbCount(), path.verbCount())
      << "verbCount mismatch. serialized: " << serialized;
  ASSERT_EQ(roundTripped.points().size(), path.points().size())
      << "points size mismatch. serialized: " << serialized;

  for (size_t i = 0; i < path.verbCount(); ++i) {
    EXPECT_EQ(roundTripped.commands()[i].verb, path.commands()[i].verb)
        << "Command " << i << " verb mismatch. serialized: " << serialized;
  }
  for (size_t i = 0; i < path.points().size(); ++i) {
    EXPECT_NEAR(roundTripped.points()[i].x, path.points()[i].x, 1e-9)
        << "Point " << i << " x mismatch. serialized: " << serialized;
    EXPECT_NEAR(roundTripped.points()[i].y, path.points()[i].y, 1e-9)
        << "Point " << i << " y mismatch. serialized: " << serialized;
  }
}

}  // namespace

TEST(PathToSVGPathDataRoundTrip, Empty) {
  ParseResult<Path> result = PathParser::Parse("");
  ASSERT_THAT(result, NoParseError());
  EXPECT_EQ(result.result().toSVGPathData(), "");
}

TEST(PathToSVGPathDataRoundTrip, MoveTo) {
  ExpectRoundTrip("M 10 20");
}

TEST(PathToSVGPathDataRoundTrip, MoveToLineTo) {
  ExpectRoundTrip("M 0 0 L 100 50");
}

TEST(PathToSVGPathDataRoundTrip, MultipleLineTo) {
  ExpectRoundTrip("M 0 0 L 100 0 L 100 100 L 0 100");
}

TEST(PathToSVGPathDataRoundTrip, ClosedTriangle) {
  ExpectRoundTrip("M 0 0 L 50 100 L 100 0 Z");
}

TEST(PathToSVGPathDataRoundTrip, CubicBezier) {
  ExpectRoundTrip("M 0 0 C 0 100 100 100 100 0");
}

TEST(PathToSVGPathDataRoundTrip, QuadraticBezier) {
  ExpectRoundTrip("M 0 0 Q 50 100 100 0");
}

TEST(PathToSVGPathDataRoundTrip, FractionalCoordinates) {
  ExpectRoundTrip("M 1.5 2.25 L 3.14 -0.5");
}

TEST(PathToSVGPathDataRoundTrip, MultipleSubpaths) {
  ExpectRoundTrip("M 0 0 L 10 0 Z M 20 20 L 30 20 Z");
}

TEST(PathToSVGPathDataRoundTrip, NegativeCoordinates) {
  ExpectRoundTrip("M -10 -20 L -5 -15");
}

TEST(PathToSVGPathDataRoundTrip, AllVerbTypes) {
  // Combine all serializable verb types in one path.
  ExpectRoundTrip("M 0 0 L 10 0 Q 15 10 20 0 C 20 20 30 20 30 0 Z");
}

TEST(PathToSVGPathDataRoundTrip, ArcDecomposedToCubic) {
  // Arcs are decomposed to cubic curves by PathBuilder::arcTo before being
  // stored in the Path.  Round-tripping the serialized cubic representation
  // must produce an equivalent Path (same verbs / points).
  //
  // The SVG 'a' command triggers that decomposition during Parse; the
  // re-serialized form uses 'C' commands and round-trips cleanly.
  ExpectRoundTrip("M 80 150 A 60 40 0 0 0 220 150");
}

}  // namespace donner::svg::parser
