#include "donner/svg/parser/TransformParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner::svg::parser {

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
              ParseErrorIs("Failed to parse number: Unexpected end of string"));
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
              ParseErrorIs("Failed to parse number: Unexpected end of string"));
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
              ParseErrorIs("Failed to parse number: Unexpected end of string"));
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

    const Transform2d t = maybeTransform.result();
    EXPECT_THAT(t.transformPosition({50, 50}), Vector2Near(50, 50));
    EXPECT_THAT(t.transformPosition({100, 50}), Vector2Near(0, 50));
    EXPECT_THAT(t.transformPosition({-100, -100}), Vector2Near(200, 200));
  }

  {
    auto maybeTransform = TransformParser::Parse("rotate ( \t 90 \r\n -50    50 ) ");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(0, 1, -1, 0, 0, 100)));

    const Transform2d t = maybeTransform.result();
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
              ParseErrorIs("Failed to parse number: Unexpected end of string"));
}

TEST(TransformParser, SkewX) {
  EXPECT_THAT(TransformParser::Parse("skewX(0)"), ParseResultIs(TransformIsIdentity()));

  {
    auto maybeTransform = TransformParser::Parse("skewX(45)");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(1, 0, 1, 1, 0, 0)));

    const Transform2d t = maybeTransform.result();
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

    const Transform2d t = maybeTransform.result();
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
              ParseErrorIs("Failed to parse number: Unexpected end of string"));
}

TEST(TransformParser, SkewY) {
  EXPECT_THAT(TransformParser::Parse("skewY(0)"), ParseResultIs(TransformIsIdentity()));

  {
    auto maybeTransform = TransformParser::Parse("skewY(45)");
    EXPECT_THAT(maybeTransform, ParseResultIs(TransformIs(1, 1, 0, 1, 0, 0)));

    const Transform2d t = maybeTransform.result();
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

    const Transform2d t = maybeTransform.result();
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
              ParseErrorIs("Failed to parse number: Unexpected end of string"));
}

TEST(TransformParser, MultiplicationOrder) {
  {
    const Transform2d t = Transform2d::Translate({-50, 100}) * Transform2d::Scale({2, 2}) *
                         Transform2d::Rotate(MathConstants<double>::kHalfPi * 0.5);

    EXPECT_THAT(TransformParser::Parse("rotate(45) scale(2) translate(-50, 100)"),
                ParseResultIs(TransformEq(t)));
  }

  {
    const Transform2d t = Transform2d::Rotate(MathConstants<double>::kHalfPi * 0.5) *
                         Transform2d::Scale({1.5, 1.5}) * Transform2d::Translate({80, 80});

    EXPECT_THAT(TransformParser::Parse("translate(80, 80), scale(1.5, 1.5) \t,\n rotate(45) "),
                ParseResultIs(TransformEq(t)));
  }
}

// ---------------------------------------------------------------------------
// Range-accuracy tests: verify that error SourceRanges cover the right span.
// ---------------------------------------------------------------------------

TEST(TransformParser, RangeUnexpectedFunction) {
  // "invalid(" => function name "invalid" starts at 0, cursor after '(' at 8 => [0,8).
  EXPECT_THAT(TransformParser::Parse("invalid("), ParseErrorRange(0, 8));
  // "invalid2()" => range covers "invalid2(" => [0,9).
  EXPECT_THAT(TransformParser::Parse("invalid2()"), ParseErrorRange(0, 9));
}

TEST(TransformParser, RangeExpectedCloseParen) {
  // "matrix(1, 2, 3, 4, 5, 6, 7)" => after parsing 6 numbers, the comma at offset 23 is
  // unexpected (expected ')').
  EXPECT_THAT(TransformParser::Parse("matrix(1, 2, 3, 4, 5, 6, 7)"), ParseErrorRange(23, 24));
}

TEST(TransformParser, RangeExpectedOpenParen) {
  // "matrix 1 2" => after "matrix", whitespace, then '1' which is not '(' => [7,8).
  EXPECT_THAT(TransformParser::Parse("matrix 1 2"), ParseErrorRange(7, 8));
}

TEST(TransformParser, RangeEndOfString) {
  // "matrix" => end of string when looking for function.
  EXPECT_THAT(TransformParser::Parse("matrix"), ParseErrorEndOfString());
  // ")" => after consuming ')', remainder is empty => EndOfString.
  EXPECT_THAT(TransformParser::Parse(")"), ParseErrorEndOfString());
}

// -----------------------------------------------------------------------------
// Round-trip test for `donner::toSVGTransformString`. The serializer lives in
// donner/base/Transform.h but can't be tested against the real parser there
// because donner/base has no dependency on donner/svg/parser. This test file
// sits on the correct side of the dep graph.
// -----------------------------------------------------------------------------

TEST(TransformParser, ToSVGTransformStringRoundTrip) {
  const Transform2d cases[] = {
      // Identity — serializes to the empty string, which the parser treats as
      // an empty input and returns identity.
      Transform2d(),
      // Translate — both forms.
      Transform2d::Translate(Vector2d(10.0, 20.0)),
      Transform2d::Translate(Vector2d(5.0, 0.0)),  // collapses to translate(5)
      Transform2d::Translate(Vector2d(1.5, -2.5)),
      Transform2d::Translate(Vector2d(-7.0, 13.0)),
      // Scale — uniform + non-uniform.
      Transform2d::Scale(2.0),
      Transform2d::Scale(Vector2d(0.5, 0.5)),
      Transform2d::Scale(Vector2d(2.0, 3.0)),
      Transform2d::Scale(Vector2d(-1.0, 1.0)),  // reflection, stays as scale
      // Rotate — 90/180/-90.
      Transform2d::Rotate(MathConstants<double>::kHalfPi),
      Transform2d::Rotate(-MathConstants<double>::kHalfPi),
      Transform2d::Rotate(MathConstants<double>::kPi),  // special case: also matches pure scale
      // Skew — falls through to matrix(...) form.
      Transform2d::SkewX(MathConstants<double>::kPi / 4.0),
      Transform2d::SkewY(MathConstants<double>::kPi / 6.0),
  };

  for (const Transform2d& original : cases) {
    const RcString serialized = toSVGTransformString(original);
    const auto result = TransformParser::Parse(serialized);
    ASSERT_TRUE(result.hasResult())
        << "Parse failed for '" << serialized << "' (from " << original << ")";
    const Transform2d parsed = result.result();
    for (int i = 0; i < 6; ++i) {
      EXPECT_NEAR(parsed.data[i], original.data[i], 1e-10)
          << "Round-trip mismatch at data[" << i << "] for '" << serialized << "'";
    }
  }
}

TEST(TransformParser, ToSVGTransformStringGeneralMatrixRoundTrip) {
  // A transform that doesn't decompose into any named form — must use matrix().
  Transform2d t(Transform2d::uninitialized);
  t.data[0] = 1.5;
  t.data[1] = 0.25;
  t.data[2] = -0.25;
  t.data[3] = 1.5;
  t.data[4] = 10.0;
  t.data[5] = 20.0;

  const RcString serialized = toSVGTransformString(t);
  EXPECT_EQ(std::string_view(serialized), "matrix(1.5, 0.25, -0.25, 1.5, 10, 20)");

  const auto result = TransformParser::Parse(serialized);
  ASSERT_TRUE(result.hasResult());
  for (int i = 0; i < 6; ++i) {
    EXPECT_NEAR(result.result().data[i], t.data[i], 1e-12);
  }
}

}  // namespace donner::svg::parser
