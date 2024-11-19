#include "donner/svg/parser/TransformParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner::svg::parser {

using namespace base::parser;  // NOLINT: For tests

TEST(TransformParser, Empty) {
  EXPECT_THAT(TransformParser::Parse(""), ParseResultIs(TransformIsIdentity()));
  EXPECT_THAT(TransformParser::Parse(" \t\r\n "), ParseResultIs(TransformIsIdentity()));
}

TEST(TransformParser, ParseErrors) {
  EXPECT_THAT(TransformParser::Parse("("), ParseErrorIs("Unexpected function ''"));
  EXPECT_THAT(TransformParser::Parse(")"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::Parse("invalid("), ParseErrorIs("Unexpected function 'invalid'"));
  EXPECT_THAT(TransformParser::Parse("invalid2()"), ParseErrorIs("Unexpected function 'invalid2'"));
  EXPECT_THAT(TransformParser::Parse("scale(1),,scale(1)"),
              ParseErrorIs("Unexpected function ',scale'"));
  EXPECT_THAT(TransformParser::Parse(",scale(1)"), ParseErrorIs("Unexpected function ',scale'"));
  EXPECT_THAT(TransformParser::Parse("()"), ParseErrorIs("Unexpected function ''"));
  EXPECT_THAT(TransformParser::Parse("scale(1))"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
}

TEST(TransformParser, Matrix) {
  EXPECT_THAT(TransformParser::Parse("matrix(1 2 3 4 5 6)"),
              ParseResultIs(TransformIs(1, 2, 3, 4, 5, 6)));

  EXPECT_THAT(TransformParser::Parse(" matrix ( \t 7 8 9 \r\n 10 11 12 ) "),
              ParseResultIs(TransformIs(7, 8, 9, 10, 11, 12)));

  EXPECT_THAT(TransformParser::Parse("matrix(-1-2-3-4-5-6)"),
              ParseResultIs(TransformIs(-1, -2, -3, -4, -5, -6)));

  EXPECT_THAT(TransformParser::Parse("matrix(6,5,4 3,2,1)"),
              ParseResultIs(TransformIs(6, 5, 4, 3, 2, 1)));
}

TEST(TransformParser, Matrix_ParseErrors) {
  // No parameters.
  EXPECT_THAT(TransformParser::Parse("matrix()"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Too few parameters.
  EXPECT_THAT(TransformParser::Parse("matrix(1, 2, 3)"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
  EXPECT_THAT(TransformParser::Parse("matrix(1, 2, 3, 4, 5)"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Too many parameters.
  EXPECT_THAT(TransformParser::Parse("matrix(1, 2, 3, 4, 5, 6, 7)"), ParseErrorIs("Expected ')'"));

  // Missing parens.
  EXPECT_THAT(TransformParser::Parse("matrix"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::Parse("matrix 1 2"),
              ParseErrorIs("Expected '(' after function name"));
  EXPECT_THAT(TransformParser::Parse("matrix("),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(TransformParser, Translate) {
  EXPECT_THAT(TransformParser::Parse("translate(2)"), ParseResultIs(TransformIs(1, 0, 0, 1, 2, 0)));

  EXPECT_THAT(TransformParser::Parse("translate(2 3)"),
              ParseResultIs(TransformIs(1, 0, 0, 1, 2, 3)));

  EXPECT_THAT(TransformParser::Parse(" translate ( \t 3 ) "),
              ParseResultIs(TransformIs(1, 0, 0, 1, 3, 0)));

  EXPECT_THAT(TransformParser::Parse(" translate ( \t 5 \r,\n 3 ) "),
              ParseResultIs(TransformIs(1, 0, 0, 1, 5, 3)));

  EXPECT_THAT(TransformParser::Parse("translate(-1-2)"),
              ParseResultIs(TransformIs(1, 0, 0, 1, -1, -2)));
}

TEST(TransformParser, Translate_ParseErrors) {
  // No parameters.
  EXPECT_THAT(TransformParser::Parse("translate()"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Bad parameter count.
  EXPECT_THAT(TransformParser::Parse("translate(2,)"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Too many parameters.
  EXPECT_THAT(TransformParser::Parse("translate(1, 2, 3)"), ParseErrorIs("Expected ')'"));
  EXPECT_THAT(TransformParser::Parse("translate(1, 2,)"), ParseErrorIs("Expected ')'"));

  // Missing parens.
  EXPECT_THAT(TransformParser::Parse("translate"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::Parse("translate 1 2"),
              ParseErrorIs("Expected '(' after function name"));
  EXPECT_THAT(TransformParser::Parse("translate("),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(TransformParser, Scale) {
  EXPECT_THAT(TransformParser::Parse("scale(2)"), ParseResultIs(TransformIs(2, 0, 0, 2, 0, 0)));

  EXPECT_THAT(TransformParser::Parse("scale(-2 3)"), ParseResultIs(TransformIs(-2, 0, 0, 3, 0, 0)));

  EXPECT_THAT(TransformParser::Parse("scale ( \t -3 ) "),
              ParseResultIs(TransformIs(-3, 0, 0, -3, 0, 0)));

  EXPECT_THAT(TransformParser::Parse("scale ( \t 5 \r,\n 3 ) "),
              ParseResultIs(TransformIs(5, 0, 0, 3, 0, 0)));

  EXPECT_THAT(TransformParser::Parse("scale(-1-2)"),
              ParseResultIs(TransformIs(-1, 0, 0, -2, 0, 0)));
}

TEST(TransformParser, Scale_ParseErrors) {
  // No parameters.
  EXPECT_THAT(TransformParser::Parse("scale()"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
  EXPECT_THAT(TransformParser::Parse("scale(,)"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Bad parameter count.
  EXPECT_THAT(TransformParser::Parse("scale(1,)"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Too many parameters.
  EXPECT_THAT(TransformParser::Parse("scale(1, 2, 3)"), ParseErrorIs("Expected ')'"));
  EXPECT_THAT(TransformParser::Parse("scale(1, 2,)"), ParseErrorIs("Expected ')'"));

  // Missing parens.
  EXPECT_THAT(TransformParser::Parse("scale"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::Parse("scale 1 2"),
              ParseErrorIs("Expected '(' after function name"));
  EXPECT_THAT(TransformParser::Parse("scale("),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(TransformParser, Rotate_OneParameter) {
  EXPECT_THAT(TransformParser::Parse("rotate(0)"), ParseResultIs(TransformIsIdentity()));
  // This is near-identity, but not close enough for isIdentity() to return true.
  EXPECT_THAT(TransformParser::Parse("rotate(360)"), ParseResultIs(TransformIs(1, 0, 0, 1, 0, 0)));
  EXPECT_THAT(TransformParser::Parse("rotate(90)"), ParseResultIs(TransformIs(0, 1, -1, 0, 0, 0)));
  EXPECT_THAT(TransformParser::Parse("rotate(180)"),
              ParseResultIs(TransformIs(-1, 0, 0, -1, 0, 0)));

  EXPECT_THAT(TransformParser::Parse("rotate ( \t -90 ) "),
              ParseResultIs(TransformIs(0, -1, 1, 0, 0, 0)));
}

TEST(TransformParser, Rotate_WithCenter) {
  // Origin offset is equivalent to not specifying one.
  EXPECT_THAT(TransformParser::Parse("rotate(0 0 0)"), ParseResultIs(TransformIsIdentity()));
  EXPECT_THAT(TransformParser::Parse("rotate(90 0 0)"),
              ParseResultIs(TransformIs(0, 1, -1, 0, 0, 0)));

  // No effect if rotation is zero.
  EXPECT_THAT(TransformParser::Parse("rotate(0 -50 12)"), ParseResultIs(TransformIsIdentity()));

  {
    auto maybeTransform = TransformParser::Parse("rotate(180 50 50)");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(-1, 0, 0, -1, 100, 100)));

    const Transformd t = maybeTransform.result();
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(50, 50));
    EXPECT_THAT(t.transformPosition({100, 50}), Vector2Near(0, 50));
    EXPECT_THAT(t.transformPosition({-100, -100}), Vector2Near(200, 200));
  }

  {
    auto maybeTransform = TransformParser::Parse("rotate ( \t 90 \r\n -50    50 ) ");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(0, 1, -1, 0, 0, 100)));

    const Transformd t = maybeTransform.result();
    EXPECT_THAT(t.transformPosition({-50, 50}), Vector2Near(-50, 50));
    EXPECT_THAT(t.transformPosition({100, 50}), Vector2Near(-50, 200));
    EXPECT_THAT(t.transformPosition({-100, -100}), Vector2Near(100, 0));
  }
}

TEST(TransformParser, Rotate_ParseErrors) {
  // No parameters.
  EXPECT_THAT(TransformParser::Parse("rotate()"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Bad parameter count.
  EXPECT_THAT(TransformParser::Parse("rotate(1,)"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
  EXPECT_THAT(TransformParser::Parse("rotate(1, 2)"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
  EXPECT_THAT(TransformParser::Parse("rotate(1, 2, )"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
  EXPECT_THAT(TransformParser::Parse("rotate(1, 2, 3, 4)"), ParseErrorIs("Expected ')'"));

  // Missing parens.
  EXPECT_THAT(TransformParser::Parse("rotate"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::Parse("rotate 1 2"),
              ParseErrorIs("Expected '(' after function name"));
  EXPECT_THAT(TransformParser::Parse("rotate("),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(TransformParser, SkewX) {
  EXPECT_THAT(TransformParser::Parse("skewX(0)"), ParseResultIs(TransformIsIdentity()));

  {
    auto maybeTransform = TransformParser::Parse("skewX(45)");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(1, 0, 1, 1, 0, 0)));

    const Transformd t = maybeTransform.result();
    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(100, 50));
    EXPECT_THAT(t.transformVector({50, 100}), Vector2Near(150, 100));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(100, 50));
    EXPECT_THAT(t.transformPosition({50, 100}), Vector2Near(150, 100));
  }

  {
    auto maybeTransform = TransformParser::Parse("skewX( \t -45 ) ");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(1, 0, -1, 1, 0, 0)));

    const Transformd t = maybeTransform.result();
    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(0, 50));
    EXPECT_THAT(t.transformVector({50, 100}), Vector2Near(-50, 100));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(0, 50));
    EXPECT_THAT(t.transformPosition({50, 100}), Vector2Near(-50, 100));
  }
}

TEST(TransformParser, SkewX_ParseErrors) {
  // No parameters.
  EXPECT_THAT(TransformParser::Parse("skewX()"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Bad parameter count.
  EXPECT_THAT(TransformParser::Parse("skewX(1,)"), ParseErrorIs("Expected ')'"));
  EXPECT_THAT(TransformParser::Parse("skewX(1, 2)"), ParseErrorIs("Expected ')'"));

  // Missing parens.
  EXPECT_THAT(TransformParser::Parse("skewX"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::Parse("skewX 1 2"),
              ParseErrorIs("Expected '(' after function name"));
  EXPECT_THAT(TransformParser::Parse("skewX("),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(TransformParser, SkewY) {
  EXPECT_THAT(TransformParser::Parse("skewY(0)"), ParseResultIs(TransformIsIdentity()));

  {
    auto maybeTransform = TransformParser::Parse("skewY(45)");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(1, 1, 0, 1, 0, 0)));

    const Transformd t = maybeTransform.result();
    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(50, 100));
    EXPECT_THAT(t.transformVector({50, 100}), Vector2Near(50, 150));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(50, 100));
    EXPECT_THAT(t.transformPosition({50, 100}), Vector2Near(50, 150));
  }

  {
    auto maybeTransform = TransformParser::Parse("skewY( \t -45 ) ");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(1, -1, 0, 1, 0, 0)));

    const Transformd t = maybeTransform.result();
    EXPECT_THAT(t.transformVector({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformVector({50, 50}), Vector2Near(50, 0));
    EXPECT_THAT(t.transformVector({100, 50}), Vector2Near(100, -50));

    EXPECT_THAT(t.transformPosition({0, 0}), Vector2Near(0, 0));
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(50, 0));
    EXPECT_THAT(t.transformPosition({100, 50}), Vector2Near(100, -50));
  }
}

TEST(TransformParser, SkewY_ParseErrors) {
  // No parameters.
  EXPECT_THAT(TransformParser::Parse("skewY()"),
              ParseErrorIs("Failed to parse number: Unexpected character"));

  // Bad parameter count.
  EXPECT_THAT(TransformParser::Parse("skewY(1,)"), ParseErrorIs("Expected ')'"));
  EXPECT_THAT(TransformParser::Parse("skewY(1, 2)"), ParseErrorIs("Expected ')'"));

  // Missing parens.
  EXPECT_THAT(TransformParser::Parse("skewY"),
              ParseErrorIs("Unexpected end of string instead of transform function"));
  EXPECT_THAT(TransformParser::Parse("skewY 1 2"),
              ParseErrorIs("Expected '(' after function name"));
  EXPECT_THAT(TransformParser::Parse("skewY("),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(TransformParser, MultiplicationOrder) {
  {
    const Transformd t = Transformd::Translate({-50, 100}) * Transformd::Scale({2, 2}) *
                         Transformd::Rotate(MathConstants<double>::kHalfPi * 0.5);

    EXPECT_THAT(TransformParser::Parse("rotate(45) scale(2) translate(-50, 100)"),
                ParseResultIs(TransformEq(t)));
  }

  {
    const Transformd t = Transformd::Rotate(MathConstants<double>::kHalfPi * 0.5) *
                         Transformd::Scale({1.5, 1.5}) * Transformd::Translate({80, 80});

    EXPECT_THAT(TransformParser::Parse("translate(80, 80), scale(1.5, 1.5) \t,\n rotate(45) "),
                ParseResultIs(TransformEq(t)));
  }
}

}  // namespace donner::svg::parser
