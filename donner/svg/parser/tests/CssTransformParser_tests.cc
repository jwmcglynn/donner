#include "donner/svg/parser/CssTransformParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/parser/ValueParser.h"

namespace donner::svg::parser {

namespace {

constexpr double kInvSqrt2 = MathConstants<double>::kInvSqrt2;

ParseResult<Transformd> parseAsCss(std::string_view str) {
  const std::vector<css::ComponentValue> components = css::parser::ValueParser::Parse(str);
  auto cssTransformResult = parser::CssTransformParser::Parse(components);
  if (cssTransformResult.hasError()) {
    return std::move(cssTransformResult.error());
  }

  return cssTransformResult.result().compute(Boxd(Vector2d(0, 0), Vector2d(800, 600)),
                                             FontMetrics());
}

}  // namespace

TEST(TransformParserCss, Empty) {
  EXPECT_THAT(parseAsCss(""), ParseResultIs(TransformIsIdentity()));
  EXPECT_THAT(parseAsCss(" \t\r\n "), ParseResultIs(TransformIsIdentity()));
}

TEST(TransformParserCss, ParseErrors) {
  EXPECT_THAT(parseAsCss("("), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss(")"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("invalid("), ParseErrorIs("Unexpected function 'invalid'"));
  EXPECT_THAT(parseAsCss("invalid2()"), ParseErrorIs("Unexpected function 'invalid2'"));
  EXPECT_THAT(parseAsCss("scale(1),,scale(1)"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss(",scale(1)"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("()"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("scale(1))"), ParseErrorIs("Expected a function, found unexpected token"));
}

TEST(TransformParserCss, Matrix) {
  EXPECT_THAT(parseAsCss("matrix(1, 2, 3, 4, 5, 6)"), ParseResultIs(TransformIs(1, 2, 3, 4, 5, 6)));

  EXPECT_THAT(parseAsCss("matrix(1, 2, 3, 4, 5, 6"), ParseResultIs(TransformIs(1, 2, 3, 4, 5, 6)))
      << "Function without ')' is permitted";

  EXPECT_THAT(parseAsCss("  matrix( 1 , 2 , 3,4, 5 ,6 ) "),
              ParseResultIs(TransformIs(1, 2, 3, 4, 5, 6)));

  EXPECT_THAT(parseAsCss("matrix(-1,-2,-3,-4,-5,-6)"),
              ParseResultIs(TransformIs(-1, -2, -3, -4, -5, -6)));
}

TEST(TransformParserCss, MatrixCaseInsensitive) {
  EXPECT_THAT(parseAsCss("mAtRiX(1, 2, 3, 4, 5, 6)"), ParseResultIs(TransformIs(1, 2, 3, 4, 5, 6)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("Matrix(1, 2, 3, 4, 5, 6)"), ParseResultIs(TransformIs(1, 2, 3, 4, 5, 6)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, MatrixParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("matrix()"), ParseErrorIs("Not enough parameters"));

  // Missing comma.
  EXPECT_THAT(parseAsCss("matrix(6,5,4 3,2,1)"), ParseErrorIs("Expected a comma"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss(" matrix ( \t 7 8 9 \r\n 10 11 12 ) "),
              ParseErrorIs("Expected a function, found unexpected token"))
      << "Spaces between ident and ( are not allowed in CSS mode";

  // Too few parameters.
  EXPECT_THAT(parseAsCss("matrix(1, 2, 3)"), ParseErrorIs("Not enough parameters"));
  EXPECT_THAT(parseAsCss("matrix(1, 2, 3, 4, 5)"), ParseErrorIs("Not enough parameters"));

  // Too many parameters.
  EXPECT_THAT(parseAsCss("matrix(1, 2, 3, 4, 5, 6, 7)"),
              ParseErrorIs("Unexpected parameters when parsing 'matrix'"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("matrix"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("matrix 1 2"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("matrix("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, Translate) {
  EXPECT_THAT(parseAsCss("translate(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 2, 0)));

  EXPECT_THAT(parseAsCss("translate(-2px"), ParseResultIs(TransformIs(1, 0, 0, 1, -2, 0)))
      << "Function without ')' is permitted";

  EXPECT_THAT(parseAsCss("translate(2px, 3px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 2, 3)));

  EXPECT_THAT(parseAsCss(" translate( \t 5px \r,\n 3px ) "),
              ParseResultIs(TransformIs(1, 0, 0, 1, 5, 3)));

  EXPECT_THAT(parseAsCss("translate(-1px,-2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, -1, -2)));
}

TEST(TransformParserCss, TranslateCaseInsensitive) {
  EXPECT_THAT(parseAsCss("tRaNsLaTe(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 2, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("Translate(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 2, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, TranslateUnits) {
  EXPECT_THAT(parseAsCss("translate(2em)"), ParseResultIs(TransformIs(1, 0, 0, 1, 32, 0)));

  EXPECT_THAT(parseAsCss("translate(50%, 75%)"), ParseResultIs(TransformIs(1, 0, 0, 1, 400, 450)));

  EXPECT_THAT(parseAsCss(" translate(72pt, 100px) "),
              ParseResultIs(TransformIs(1, 0, 0, 1, 96, 100)));
}

TEST(TransformParserCss, TranslateParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("translate()"), ParseErrorIs("Not enough parameters"));

  // Invalid <length-percentage>.
  EXPECT_THAT(parseAsCss("translate(2)"), ParseErrorIs("Invalid length or percentage"));

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("translate(2px,)"), ParseErrorIs("Not enough parameters"));

  // Missing comma.
  EXPECT_THAT(parseAsCss("translate(2px 4px)"), ParseErrorIs("Expected a comma"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss(" translate ( \t 3px ) "),
              ParseErrorIs("Expected a function, found unexpected token"));

  // Too many parameters.
  EXPECT_THAT(parseAsCss("translate(1px, 2px, 3px)"),
              ParseErrorIs("Unexpected parameters when parsing 'translate'"));
  EXPECT_THAT(parseAsCss("translate(1px, 2px,)"),
              ParseErrorIs("Unexpected parameters when parsing 'translate'"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("translate"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("translate 1px 2px"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("translate("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, TranslateX) {
  EXPECT_THAT(parseAsCss("translateX(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 2, 0)));

  EXPECT_THAT(parseAsCss("translateX( \t -3px ) "), ParseResultIs(TransformIs(1, 0, 0, 1, -3, 0)));

  EXPECT_THAT(parseAsCss("translateX(4px"), ParseResultIs(TransformIs(1, 0, 0, 1, 4, 0)))
      << "Function without ')' is permitted";
}

TEST(TransformParserCss, TranslateXCaseInsensitive) {
  EXPECT_THAT(parseAsCss("tRaNsLaTeX(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 2, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("TranslateX(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 2, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, TranslateXParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("translateX()"), ParseErrorIs("Not enough parameters"));

  // Invalid <length-percentage>.
  EXPECT_THAT(parseAsCss("translateX(2)"), ParseErrorIs("Invalid length or percentage"));

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("translateX(2px,)"), ParseErrorIs("Expected only one parameter"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss(" translateX ( \t 3px ) "),
              ParseErrorIs("Expected a function, found unexpected token"));

  // Too many parameters.
  EXPECT_THAT(parseAsCss("translateX(1px, 2px)"), ParseErrorIs("Expected only one parameter"));
  EXPECT_THAT(parseAsCss("translateX(1px, )"), ParseErrorIs("Expected only one parameter"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("translateX"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("translateX 1px"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("translateX("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, TranslateY) {
  EXPECT_THAT(parseAsCss("translateY(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 0, 2)));

  EXPECT_THAT(parseAsCss("translateY( \t -3px ) "), ParseResultIs(TransformIs(1, 0, 0, 1, 0, -3)));

  EXPECT_THAT(parseAsCss("translateY(4px"), ParseResultIs(TransformIs(1, 0, 0, 1, 0, 4)))
      << "Function without ')' is permitted";
}

TEST(TransformParserCss, TranslateYCaseInsensitive) {
  EXPECT_THAT(parseAsCss("tRaNsLaTeY(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 0, 2)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("TranslateY(2px)"), ParseResultIs(TransformIs(1, 0, 0, 1, 0, 2)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, TranslateYParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("translateY()"), ParseErrorIs("Not enough parameters"));

  // Invalid <length-percentage>.
  EXPECT_THAT(parseAsCss("translateY(2)"), ParseErrorIs("Invalid length or percentage"));

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("translateY(2px,)"), ParseErrorIs("Expected only one parameter"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss(" translateY ( \t 3px ) "),
              ParseErrorIs("Expected a function, found unexpected token"));

  // Too many parameters.
  EXPECT_THAT(parseAsCss("translateY(1px, 2px)"), ParseErrorIs("Expected only one parameter"));
  EXPECT_THAT(parseAsCss("translateY(1px, )"), ParseErrorIs("Expected only one parameter"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("translateY"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("translateY 1px"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("translateY("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, Scale) {
  EXPECT_THAT(parseAsCss("scale(2)"), ParseResultIs(TransformIs(2, 0, 0, 2, 0, 0)));

  EXPECT_THAT(parseAsCss("scale(4"), ParseResultIs(TransformIs(4, 0, 0, 4, 0, 0)))
      << "Function without ')' is permitted";

  EXPECT_THAT(parseAsCss("scale(-2, 3)"), ParseResultIs(TransformIs(-2, 0, 0, 3, 0, 0)));

  EXPECT_THAT(parseAsCss("scale( \t -3 ) "), ParseResultIs(TransformIs(-3, 0, 0, -3, 0, 0)));

  EXPECT_THAT(parseAsCss("scale( \t 5 \r,\n 3 ) "), ParseResultIs(TransformIs(5, 0, 0, 3, 0, 0)));

  EXPECT_THAT(parseAsCss("scale(-1,-2)"), ParseResultIs(TransformIs(-1, 0, 0, -2, 0, 0)));
}

TEST(TransformParserCss, ScaleCaseInsensitive) {
  EXPECT_THAT(parseAsCss("sCaLe(2)"), ParseResultIs(TransformIs(2, 0, 0, 2, 0, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("Scale(2)"), ParseResultIs(TransformIs(2, 0, 0, 2, 0, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, ScaleParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("scale()"), ParseErrorIs("Not enough parameters"));
  EXPECT_THAT(parseAsCss("scale(,)"), ParseErrorIs("Expected a number"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss("scale (-3)"), ParseErrorIs("Expected a function, found unexpected token"))
      << "Spaces between ident and ( are not allowed in CSS mode";

  // Missing a comma.
  EXPECT_THAT(parseAsCss("scale(-2 3)"), ParseErrorIs("Expected a comma"));

  // Too many commas.
  EXPECT_THAT(parseAsCss("scale(-2,,3)"), ParseErrorIs("Expected a number"));

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("scale(1,)"), ParseErrorIs("Not enough parameters"));

  // Too many parameters.
  EXPECT_THAT(parseAsCss("scale(1, 2, 3)"),
              ParseErrorIs("Unexpected parameters when parsing 'scale'"));
  EXPECT_THAT(parseAsCss("scale(1, 2,)"),
              ParseErrorIs("Unexpected parameters when parsing 'scale'"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("scale"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("scale 1 2"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("scale("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, ScaleX) {
  EXPECT_THAT(parseAsCss("scaleX(2)"), ParseResultIs(TransformIs(2, 0, 0, 1, 0, 0)));
  EXPECT_THAT(parseAsCss("scaleX( \t -3 ) "), ParseResultIs(TransformIs(-3, 0, 0, 1, 0, 0)));

  EXPECT_THAT(parseAsCss("scaleX(4"), ParseResultIs(TransformIs(4, 0, 0, 1, 0, 0)))
      << "Function without ')' is permitted";
}

TEST(TransformParserCss, ScaleXCaseInsensitive) {
  EXPECT_THAT(parseAsCss("sCaLeX(2)"), ParseResultIs(TransformIs(2, 0, 0, 1, 0, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("ScaleX(2)"), ParseResultIs(TransformIs(2, 0, 0, 1, 0, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, ScaleXParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("scaleX()"), ParseErrorIs("Not enough parameters"));
  EXPECT_THAT(parseAsCss("scaleX(,)"), ParseErrorIs("Expected a number"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss("scaleX (-3)"),
              ParseErrorIs("Expected a function, found unexpected token"))
      << "Spaces between ident and ( are not allowed in CSS mode";

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("scaleX(1,)"), ParseErrorIs("Expected only one parameter"));

  // Too many parameters.
  EXPECT_THAT(parseAsCss("scaleX(1, 2)"), ParseErrorIs("Expected only one parameter"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("scaleX"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("scaleX 1"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("scaleX("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, ScaleY) {
  EXPECT_THAT(parseAsCss("scaleY(2)"), ParseResultIs(TransformIs(1, 0, 0, 2, 0, 0)));
  EXPECT_THAT(parseAsCss("scaleY( \t -3 ) "), ParseResultIs(TransformIs(1, 0, 0, -3, 0, 0)));

  EXPECT_THAT(parseAsCss("scaleY(4"), ParseResultIs(TransformIs(1, 0, 0, 4, 0, 0)))
      << "Function without ')' is permitted";
}

TEST(TransformParserCss, ScaleYCaseInsensitive) {
  EXPECT_THAT(parseAsCss("sCaLeY(2)"), ParseResultIs(TransformIs(1, 0, 0, 2, 0, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("ScaleY(2)"), ParseResultIs(TransformIs(1, 0, 0, 2, 0, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, ScaleYParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("scaleY()"), ParseErrorIs("Not enough parameters"));
  EXPECT_THAT(parseAsCss("scaleY(,)"), ParseErrorIs("Expected a number"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss("scaleY (-3)"),
              ParseErrorIs("Expected a function, found unexpected token"))
      << "Spaces between ident and ( are not allowed in CSS mode";

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("scaleY(1,)"), ParseErrorIs("Expected only one parameter"));

  // Too many parameters.
  EXPECT_THAT(parseAsCss("scaleY(1, 2)"), ParseErrorIs("Expected only one parameter"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("scaleY"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("scaleY 1"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("scaleY("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, Rotate) {
  EXPECT_THAT(parseAsCss("rotate(0)"), ParseResultIs(TransformIsIdentity()));
  EXPECT_THAT(parseAsCss("rotate(45deg)"),
              ParseResultIs(TransformIs(kInvSqrt2, kInvSqrt2, -kInvSqrt2, kInvSqrt2, 0, 0)));
  // This is near-identity, but not close enough for isIdentity() to return true.
  EXPECT_THAT(parseAsCss("rotate(360deg)"), ParseResultIs(TransformIs(1, 0, 0, 1, 0, 0)));
  EXPECT_THAT(parseAsCss("rotate(90deg)"), ParseResultIs(TransformIs(0, 1, -1, 0, 0, 0)));
  EXPECT_THAT(parseAsCss("rotate(180deg)"), ParseResultIs(TransformIs(-1, 0, 0, -1, 0, 0)));

  EXPECT_THAT(parseAsCss("rotate( \t -90deg ) "), ParseResultIs(TransformIs(0, -1, 1, 0, 0, 0)));
}

TEST(TransformParserCss, RotateCaseInsensitive) {
  EXPECT_THAT(parseAsCss("rOtAtE(45deg)"),
              ParseResultIs(TransformIs(kInvSqrt2, kInvSqrt2, -kInvSqrt2, kInvSqrt2, 0, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("Rotate(45deg)"),
              ParseResultIs(TransformIs(kInvSqrt2, kInvSqrt2, -kInvSqrt2, kInvSqrt2, 0, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, RotateUnits) {
  EXPECT_THAT(parseAsCss("rotate(0.5turn)"), ParseResultIs(TransformIs(-1, 0, 0, -1, 0, 0)));
  EXPECT_THAT(parseAsCss("rotate(3.14159265359rad)"),
              ParseResultIs(TransformIs(-1, 0, 0, -1, 0, 0)));
  EXPECT_THAT(parseAsCss("rotate(200grad)"), ParseResultIs(TransformIs(-1, 0, 0, -1, 0, 0)));
}

TEST(TransformParserCss, RotateParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("rotate()"), ParseErrorIs("Not enough parameters"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss("rotate ( \t -90deg ) "),
              ParseErrorIs("Expected a function, found unexpected token"))
      << "Spaces between ident and ( are not allowed in CSS mode";

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("rotate(1deg,)"), ParseErrorIs("Expected only one parameter"));
  EXPECT_THAT(parseAsCss("rotate(1deg, 2deg)"), ParseErrorIs("Expected only one parameter"));
  EXPECT_THAT(parseAsCss("rotate(1deg, 2deg, )"), ParseErrorIs("Expected only one parameter"));
  EXPECT_THAT(parseAsCss("rotate(1deg, 2deg, 3deg, 4deg)"),
              ParseErrorIs("Expected only one parameter"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("rotate"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("rotate 1deg, 2deg"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("rotate("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, Skew) {
  EXPECT_THAT(parseAsCss("skew(2deg)"), ParseResultIs(TransformIs(1, 0, 0.0349208, 1, 0, 0)));

  EXPECT_THAT(parseAsCss("skew(4deg"), ParseResultIs(TransformIs(1, 0, 0.0699268, 1, 0, 0)))
      << "Function without ')' is permitted";

  EXPECT_THAT(parseAsCss("skew(-2deg, 3deg)"),
              ParseResultIs(TransformIs(1, 0.0524078, -0.0349208, 1, 0, 0)));

  EXPECT_THAT(parseAsCss("skew( \t -3deg ) "),
              ParseResultIs(TransformIs(1, 0, -0.0524078, 1, 0, 0)));

  EXPECT_THAT(parseAsCss("skew( \t 5deg \r,\n 3deg ) "),
              ParseResultIs(TransformIs(1, 0.0524078, 0.0874887, 1, 0, 0)));

  EXPECT_THAT(parseAsCss("skew(-1deg,-2deg)"),
              ParseResultIs(TransformIs(1, -0.0349208, -0.0174551, 1, 0, 0)));
}

TEST(TransformParserCss, SkewCaseInsensitive) {
  EXPECT_THAT(parseAsCss("sKeW(45deg)"), ParseResultIs(TransformIs(1, 0, 1, 1, 0, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("Skew(45deg)"), ParseResultIs(TransformIs(1, 0, 1, 1, 0, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, SkewParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("skew()"), ParseErrorIs("Not enough parameters"));
  EXPECT_THAT(parseAsCss("skew(,)"), ParseErrorIs("Invalid angle"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss("skew (-3deg)"),
              ParseErrorIs("Expected a function, found unexpected token"))
      << "Spaces between ident and ( are not allowed in CSS mode";

  // Missing a comma.
  EXPECT_THAT(parseAsCss("skew(-2deg 3deg)"), ParseErrorIs("Expected a comma"));

  // Too many commas.
  EXPECT_THAT(parseAsCss("skew(-2deg,,3deg)"), ParseErrorIs("Invalid angle"));

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("skew(1deg,)"), ParseErrorIs("Not enough parameters"));

  // Too many parameters.
  EXPECT_THAT(parseAsCss("skew(1deg, 2deg, 3deg)"),
              ParseErrorIs("Unexpected parameters when parsing 'skew'"));
  EXPECT_THAT(parseAsCss("skew(1deg, 2deg,)"),
              ParseErrorIs("Unexpected parameters when parsing 'skew'"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("skew"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("skew 1deg 2deg"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("skew("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, SkewX) {
  EXPECT_THAT(parseAsCss("skewX(0)"), ParseResultIs(TransformIsIdentity()));
  EXPECT_THAT(parseAsCss("skewX(0"), ParseResultIs(TransformIsIdentity()))
      << "Function without ')' is permitted";

  {
    auto maybeTransform = parseAsCss("skewX(45deg)");
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
    auto maybeTransform = parseAsCss("skewX( \t -45deg ) ");
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

TEST(TransformParserCss, SkewXCaseInsensitive) {
  EXPECT_THAT(parseAsCss("sKeWx(45deg)"), ParseResultIs(TransformIs(1, 0, 1, 1, 0, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("SkewX(45deg)"), ParseResultIs(TransformIs(1, 0, 1, 1, 0, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, SkewXParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("skewX()"), ParseErrorIs("Not enough parameters"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss("skewX ( \t -45deg ) "),
              ParseErrorIs("Expected a function, found unexpected token"))
      << "Spaces between ident and ( are not allowed in CSS mode";

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("skewX(1deg,)"), ParseErrorIs("Expected only one parameter"));
  EXPECT_THAT(parseAsCss("skewX(1deg, 2deg)"), ParseErrorIs("Expected only one parameter"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("skewX"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("skewX 1deg 2deg"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("skewX("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, SkewY) {
  EXPECT_THAT(parseAsCss("skewY(0)"), ParseResultIs(TransformIsIdentity()));
  EXPECT_THAT(parseAsCss("skewY(0"), ParseResultIs(TransformIsIdentity()))
      << "Function without ')' is permitted";

  {
    auto maybeTransform = parseAsCss("skewY(45deg)");
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
    auto maybeTransform = parseAsCss("skewY( \t -45deg ) ");
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

TEST(TransformParserCss, SkewYCaseInsensitive) {
  EXPECT_THAT(parseAsCss("sKeWy(45deg)"), ParseResultIs(TransformIs(1, 1, 0, 1, 0, 0)))
      << "Function name is case-insensitive";
  EXPECT_THAT(parseAsCss("SkewY(45deg)"), ParseResultIs(TransformIs(1, 1, 0, 1, 0, 0)))
      << "Function name is case-insensitive";
}

TEST(TransformParserCss, SkewYParseErrors) {
  // No parameters.
  EXPECT_THAT(parseAsCss("skewY()"), ParseErrorIs("Not enough parameters"));

  // Invalid spacing.
  EXPECT_THAT(parseAsCss("skewY ( \t -45deg ) "),
              ParseErrorIs("Expected a function, found unexpected token"))
      << "Spaces between ident and ( are not allowed in CSS mode";

  // Bad parameter count.
  EXPECT_THAT(parseAsCss("skewY(1deg,)"), ParseErrorIs("Expected only one parameter"));
  EXPECT_THAT(parseAsCss("skewY(1deg, 2deg)"), ParseErrorIs("Expected only one parameter"));

  // Missing parens.
  EXPECT_THAT(parseAsCss("skewY"), ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("skewY 1deg 2deg"),
              ParseErrorIs("Expected a function, found unexpected token"));
  EXPECT_THAT(parseAsCss("skewY("), ParseErrorIs("Not enough parameters"));
}

TEST(TransformParserCss, MultiplicationOrder) {
  {
    const Transformd t = Transformd::Translate({-50, 100}) * Transformd::Scale({2, 2}) *
                         Transformd::Rotate(MathConstants<double>::kHalfPi * 0.5);

    EXPECT_THAT(parseAsCss("rotate(45deg) scale(2) translate(-50px, 100px)"),
                ParseResultIs(TransformEq(t)));
  }

  {
    const Transformd t = Transformd::Rotate(MathConstants<double>::kHalfPi * 0.5) *
                         Transformd::Scale({1.5, 1.5}) * Transformd::Translate({80, 80});

    EXPECT_THAT(parseAsCss("translate(80px, 80px) scale(1.5, 1.5) \n rotate(45deg) "),
                ParseResultIs(TransformEq(t)));
  }
}

TEST(TransformParserCss, CompositeCaseSensitivity) {
  EXPECT_THAT(
      parseAsCss("ScAlE(2) TrAnSlAtE(2px) RoTaTe(45deg)"),
      ParseResultIs(TransformIs(2 * kInvSqrt2, 2 * kInvSqrt2, -2 * kInvSqrt2, 2 * kInvSqrt2, 4, 0)))
      << "Function names are case-insensitive";
  EXPECT_THAT(parseAsCss("sKeW(45deg) sKeWx(45deg) sKeWy(45deg)"),
              ParseResultIs(TransformIs(1, 1, 2, 3, 0, 0)))
      << "Function names are case-insensitive";
}

}  // namespace donner::svg::parser
