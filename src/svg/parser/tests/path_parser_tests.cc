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

TEST(PathParser, InitialMoveTo) {
  EXPECT_THAT(PathParser::parse("M"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::parse("M 0"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::parse("M0 0"), NoParseError());
  EXPECT_THAT(PathParser::parse("M0,0"), NoParseError());
  EXPECT_THAT(PathParser::parse("M0\n,\t0"), NoParseError());

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

TEST(PathParser, MoveTo) {
  {
    ParseResult<PathSpline> result = PathParser::parse("M 0 0 1 1 M 2 2 0 0");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0),
                                             Vector2d(2.0, 2.0), Vector2d::Zero()));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::MoveTo, 2}, Command{CommandType::LineTo, 3}));
  }
}

TEST(PathParser, ClosePath) {
  // Use z and Z interchangeably, they should be equivalent.

  // Immediate ClosePath.
  {
    ParseResult<PathSpline> result = PathParser::parse("M 0 0 z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero()));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::ClosePath, 0}));
  }

  // ClosePath without any additional commands should have the last MoveTo stripped.
  {
    ParseResult<PathSpline> result = PathParser::parse("M 0 0 1 1 Z");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d::Zero(), Vector2d(1.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                            Command{CommandType::ClosePath, 0}));
  }

  // ClosePath followed by a line, contains a MoveTo then a LineTo.
  {
    ParseResult<PathSpline> result = PathParser::parse("M 0 0 1 1 z L -1 -1");
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
    ParseResult<PathSpline> result = PathParser::parse("M 0 0 1 1 Z M -2 -2 -1 -1");
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
    ParseResult<PathSpline> result = PathParser::parse("M0,0Z,");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d::Zero()),
                                                 ElementsAre(Command{CommandType::MoveTo, 0},
                                                             Command{CommandType::ClosePath, 0})),
                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  // No numbers at end, there is no implicit command after.
  {
    ParseResult<PathSpline> result = PathParser::parse("M0,0Z1");

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
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 L 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase l -> relative LineTo
  {
    ParseResult<PathSpline> result = PathParser::parse("m 1 1 l 2 3");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 4.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Chain without additional letters.
  {
    ParseResult<PathSpline> result = PathParser::parse("M 0 0 L 1 0 0 1");
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
    ParseResult<PathSpline> result = PathParser::parse("M0,0L1,0,0,1");
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
    ParseResult<PathSpline> result = PathParser::parse("M 0 0 L 1 0 l 1 1 L 0 0");
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
  EXPECT_THAT(PathParser::parse("M0,0 1"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::parse("M0,0 1"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::parse("M0,0 1,"), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::parse("M0,0 1, "), ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::parse("M0,0 1,1"), NoParseError());

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

TEST(PathParser, LineTo_PartialParse) {
  {
    ParseResult<PathSpline> result = PathParser::parse("M1,1 2,3,");

    EXPECT_THAT(result, ParseResultAndError(PointsAndCommandsAre(
                                                ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 3.0)),
                                                ElementsAre(Command{CommandType::MoveTo, 0},
                                                            Command{CommandType::LineTo, 1})),
                                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  {
    ParseResult<PathSpline> result = PathParser::parse("M1,1 2,3, 4,");

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
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 H 2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase h -> relative HorizontalLineTo
  {
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 h 2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(3.0, 1.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Chain between multiple types.
  {
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 h 1 h -6 H 0 H -2 h -1");
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
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 h 1 2 3");
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
    ParseResult<PathSpline> result = PathParser::parse("M1,1h1,2,3");
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
    ParseResult<PathSpline> result = PathParser::parse("M1,1 h1,");

    EXPECT_THAT(result, ParseResultAndError(PointsAndCommandsAre(
                                                ElementsAre(Vector2d(1.0, 1.0), Vector2d(2.0, 1.0)),
                                                ElementsAre(Command{CommandType::MoveTo, 0},
                                                            Command{CommandType::LineTo, 1})),
                                            ParseErrorIs("Unexpected ',' at end of string")));
  }

  {
    ParseResult<PathSpline> result = PathParser::parse("M1 1 h");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Invalid argument")));
  }

  {
    ParseResult<PathSpline> result = PathParser::parse("M1 1 h,");

    EXPECT_THAT(result, ParseResultAndError(
                            PointsAndCommandsAre(ElementsAre(Vector2d(1.0, 1.0)),
                                                 ElementsAre(Command{CommandType::MoveTo, 0})),
                            ParseErrorIs("Failed to parse number: Invalid argument")));
  }
}

TEST(PathParser, VerticalLineTo) {
  // Uppercase V -> absolute VerticalLineTo
  {
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 V 2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 2.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Lowercase v -> relative VerticalLineTo
  {
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 v 2");
    ASSERT_THAT(result, NoParseError());

    PathSpline spline = result.result();
    EXPECT_THAT(spline.points(), ElementsAre(Vector2d(1.0, 1.0), Vector2d(1.0, 3.0)));
    EXPECT_THAT(spline.commands(),
                ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1}));
  }

  // Chain between multiple types.
  {
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 v 1 v -6 V 0 V -2 v -1");
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
    ParseResult<PathSpline> result = PathParser::parse("M 1 1 v 1 2 3");
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
    ParseResult<PathSpline> result = PathParser::parse("M1,1v1,2,3");
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
        PathParser::parse("M100,200 C100,100 250,100 250,200 S400,300 400,200");
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
}

TEST(PathParser, QuadCurveTo) {
  {
    ParseResult<PathSpline> result = PathParser::parse("M200,300 Q400,50 600,300 T1000,300");
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

    ParseResult<PathSpline> result = PathParser::parse("M300,200 h-150 a150,150 0 1,0 150,-150 z");
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

    ParseResult<PathSpline> result = PathParser::parse("M275,175 v-150 A150,150 0 0,0 125,175 z");
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

TEST(PathParser, EllipticalArc_Parsing) {
  // Missing rotation.
  EXPECT_THAT(PathParser::parse("M0,0 a150,150"),
              ParseErrorIs(HasSubstr("Failed to parse number")));

  // Missing flag.
  EXPECT_THAT(PathParser::parse("M0,0 a150,150 0"),
              ParseErrorIs("Unexpected end of string when parsing flag"));
  EXPECT_THAT(PathParser::parse("M0,0 a150,150 0,"),
              ParseErrorIs("Unexpected end of string when parsing flag"));

  // Invalid flag.
  EXPECT_THAT(PathParser::parse("M0,0 a150,150 0 a"),
              ParseErrorIs(HasSubstr("Unexpected character when parsing flag")));
  EXPECT_THAT(PathParser::parse("M0,0 a150,150 0 2"),
              ParseErrorIs(HasSubstr("Unexpected character when parsing flag")));

  // Missing end point.
  EXPECT_THAT(PathParser::parse("M0,0 a150,150 0 0,0"),
              ParseErrorIs(HasSubstr("Failed to parse number")));
  EXPECT_THAT(PathParser::parse("M0,0 a150,150 0 0,0 150"),
              ParseErrorIs(HasSubstr("Failed to parse number")));

  // No whitespace.
  EXPECT_THAT(PathParser::parse("M0,0 a150,150,0,0,0,150,150"), NoParseError());
}

}  // namespace donner
