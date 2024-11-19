#include "donner/css/parser/ColorParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/css/Color.h"

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

namespace donner::css::parser {

using namespace base::parser;  // NOLINT: For tests

TEST(Color, ColorPrintTo) {
  using string_literals::operator""_rgb;
  using string_literals::operator""_rgba;

  EXPECT_EQ(testing::PrintToString(Color(RGBA(0x11, 0x22, 0x33, 0x44))),
            "Color(rgba(17, 34, 51, 68))");
  EXPECT_EQ(testing::PrintToString(Color(Color::CurrentColor())), "Color(currentColor)");

  EXPECT_EQ(testing::PrintToString(0xFFFFFF_rgb), "Color(rgba(255, 255, 255, 255))");
  EXPECT_EQ(testing::PrintToString(0x000000_rgb), "Color(rgba(0, 0, 0, 255))");
  EXPECT_EQ(testing::PrintToString(0x123456_rgb), "Color(rgba(18, 52, 86, 255))");

  EXPECT_EQ(testing::PrintToString(0xFFFFFF00_rgba), "Color(rgba(255, 255, 255, 0))");
  EXPECT_EQ(testing::PrintToString(0x000000CC_rgba), "Color(rgba(0, 0, 0, 204))");
  EXPECT_EQ(testing::PrintToString(0x12345678_rgba), "Color(rgba(18, 52, 86, 120))");
}

TEST(ColorParser, Empty) {
  EXPECT_THAT(ColorParser::Parse({}), ParseErrorIs("No color found"));
  EXPECT_THAT(ColorParser::ParseString(""), ParseErrorIs("No color found"));
}

TEST(ColorParser, ByName) {
  EXPECT_THAT(ColorParser::Parse(std::initializer_list<ComponentValue>{
                  ComponentValue(Token(Token::Ident("blue"), 0))  //
              }),
              ParseResultIs(Color(RGBA(0, 0, 255, 255))));
  EXPECT_THAT(ColorParser::ParseString("blue"), ParseResultIs(Color(RGBA(0, 0, 255, 255))));

  // Named colors are ASCII case-insensitive.
  EXPECT_THAT(ColorParser::ParseString("bLuE"), ParseResultIs(Color(RGBA(0, 0, 255, 255))));
  EXPECT_THAT(ColorParser::ParseString("Transparent"), ParseResultIs(Color(RGBA(0, 0, 0, 0))));
  EXPECT_THAT(ColorParser::ParseString("CornflowerBlue"),
              ParseResultIs(Color(RGBA(100, 149, 237, 255))));

  // Invalid colors generate a parse error.
  EXPECT_THAT(ColorParser::Parse(std::initializer_list<ComponentValue>{
                  ComponentValue(Token(Token::Ident("test"), 0))  //
              }),
              ParseErrorIs("Invalid color 'test'"));
  EXPECT_THAT(ColorParser::ParseString("test"), ParseErrorIs("Invalid color 'test'"));
}

TEST(ColorParser, Hash) {
  EXPECT_THAT(ColorParser::Parse(std::initializer_list<ComponentValue>{
                  ComponentValue(Token(Token::Hash(Token::Hash::Type::Id, "0000FF"), 0))  //
              }),
              ParseResultIs(Color(RGBA(0, 0, 255, 255))));
  EXPECT_THAT(ColorParser::ParseString("#FF0000"), ParseResultIs(Color(RGBA(255, 0, 0, 255))));

  EXPECT_THAT(ColorParser::ParseString("#FFF"), ParseResultIs(Color(RGBA(255, 255, 255, 255))));
  EXPECT_THAT(ColorParser::ParseString("#ABCD"),
              ParseResultIs(Color(RGBA(0xAA, 0xBB, 0xCC, 0xDD))));
  EXPECT_THAT(ColorParser::ParseString("#ABCdef"),
              ParseResultIs(Color(RGBA(0xAB, 0xCD, 0xEF, 0xFF))));
  EXPECT_THAT(ColorParser::ParseString("#abcDEF"),
              ParseResultIs(Color(RGBA(0xAB, 0xCD, 0xEF, 0xFF))));

  EXPECT_THAT(ColorParser::ParseString("#112233"),
              ParseResultIs(Color(RGBA(0x11, 0x22, 0x33, 0xFF))));
  EXPECT_THAT(ColorParser::ParseString("#11223344"),
              ParseResultIs(Color(RGBA(0x11, 0x22, 0x33, 0x44))));
}

TEST(ColorParser, Whitespace) {
  EXPECT_THAT(ColorParser::Parse(std::initializer_list<ComponentValue>{
                  ComponentValue(Token(Token::Whitespace(" \t"), 0)),
                  ComponentValue(Token(Token::Hash(Token::Hash::Type::Id, "0000FF"), 0))  //
              }),
              ParseResultIs(Color(RGBA(0, 0, 255, 255))));
  EXPECT_THAT(ColorParser::ParseString(" \t  #FF0000  "),
              ParseResultIs(Color(RGBA(255, 0, 0, 255))));
  EXPECT_THAT(ColorParser::ParseString("\nblue"), ParseResultIs(Color(RGBA(0, 0, 255, 255))));
}

TEST(ColorParser, ExtraTokens) {
  EXPECT_THAT(ColorParser::ParseString(" \t  #FF0000  abc "),
              ParseErrorIs("Expected a single color"));
}

TEST(ColorParser, InvalidHash) {
  EXPECT_THAT(ColorParser::ParseString("#"), ParseErrorIs("Unexpected token when parsing color"));
  EXPECT_THAT(ColorParser::ParseString("#G"), ParseErrorIs("'#G' is not a hex number"));
  EXPECT_THAT(ColorParser::ParseString("#GHI"), ParseErrorIs("'#GHI' is not a hex number"));

  EXPECT_THAT(ColorParser::ParseString("#A"), ParseErrorIs("'#A' is not a color"));
  EXPECT_THAT(ColorParser::ParseString("#AB"), ParseErrorIs("'#AB' is not a color"));
  // 3 and 4 are valid.
  EXPECT_THAT(ColorParser::ParseString("#ABCDE"), ParseErrorIs("'#ABCDE' is not a color"));
  // 6 is valid.
  EXPECT_THAT(ColorParser::ParseString("#1234567"), ParseErrorIs("'#1234567' is not a color"));
  // 8 is valid.
  EXPECT_THAT(ColorParser::ParseString("#123456789"), ParseErrorIs("'#123456789' is not a color"));
}

TEST(ColorParser, Block) {
  EXPECT_THAT(ColorParser::ParseString("{ block }"),
              ParseErrorIs("Unexpected block when parsing color"));
}

TEST(ColorParser, FunctionNotImplemented) {
  EXPECT_THAT(ColorParser::ParseString("color(1,2,3)"), ParseErrorIs("Not implemented"));
  EXPECT_THAT(ColorParser::ParseString("device-cmyk(1,2,3)"), ParseErrorIs("Not implemented"));
}

TEST(ColorParser, FunctionError) {
  EXPECT_THAT(ColorParser::ParseString("not-supported(1,2,3)"),
              ParseErrorIs("Unsupported color function 'not-supported'"));
  EXPECT_THAT(ColorParser::ParseString("_(1,2,3)"), ParseErrorIs("Unsupported color function '_'"));

  EXPECT_THAT(ColorParser::ParseString("rgb({})"),
              ParseErrorIs("Unexpected token when parsing function 'rgb'"));
}

TEST(ColorParser, TrySkipSlash) {
  // Found slash.
  EXPECT_THAT(ColorParser::ParseString("rgb(20% 10% 5% / 50%)"),
              ParseResultIs(Color(RGBA(51, 26, 13, 127))));
  // Invalid tokens.
  EXPECT_THAT(ColorParser::ParseString("rgb(20% 10% 5% , 50%)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(20% 10% 5% {})"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(20% 10% 5% ;)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'rgb'"));
}

TEST(ColorParser, Rgb) {
  // Validate pure RGB.
  EXPECT_THAT(ColorParser::ParseString("rgb(1,2, 3)"), ParseResultIs(Color(RGBA(1, 2, 3, 255))));
  EXPECT_THAT(ColorParser::ParseString("rgb(3 4 5)"), ParseResultIs(Color(RGBA(3, 4, 5, 255))));

  // rgba is an alias for rgb.
  EXPECT_THAT(ColorParser::ParseString("rgba(1,2, 3)"), ParseResultIs(Color(RGBA(1, 2, 3, 255))));
  EXPECT_THAT(ColorParser::ParseString("rgba(3 4 5)"), ParseResultIs(Color(RGBA(3, 4, 5, 255))));

  // Errors if commas are inconsistent.
  EXPECT_THAT(ColorParser::ParseString("rgb(3 4, 5)"),
              ParseErrorIs("Unexpected token when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(3, 4 5)"),
              ParseErrorIs("Missing comma when parsing function 'rgb'"));

  // With alpha.
  EXPECT_THAT(ColorParser::ParseString("rgb(1, 2, 3, 0.02)"),
              ParseResultIs(Color(RGBA(1, 2, 3, 5))));
  EXPECT_THAT(ColorParser::ParseString("rgb(5 6 7 / 8%)"), ParseResultIs(Color(RGBA(5, 6, 7, 20))));

  // Invalid alpha.
  EXPECT_THAT(ColorParser::ParseString("rgb(5 6 7 / 5in)"), ParseErrorIs("Unexpected alpha value"));

  // Alpha is clamped.
  EXPECT_THAT(ColorParser::ParseString("rgb(1, 2, 3, 2)"),
              ParseResultIs(Color(RGBA(1, 2, 3, 255))));
  EXPECT_THAT(ColorParser::ParseString("rgb(1, 2, 3, -1)"), ParseResultIs(Color(RGBA(1, 2, 3, 0))));

  // Percentages.
  EXPECT_THAT(ColorParser::ParseString("rgb(50%, 30%, 10%)"),
              ParseResultIs(Color(RGBA(127, 77, 26, 255))));
  EXPECT_THAT(ColorParser::ParseString("rgb( 5% 10% 20% )"),
              ParseResultIs(Color(RGBA(13, 26, 51, 255))));

  EXPECT_THAT(ColorParser::ParseString("rgb( 1%, 10%, 30%, 80% )"),
              ParseResultIs(Color(RGBA(3, 26, 77, 204))));
  EXPECT_THAT(ColorParser::ParseString("rgb(20% 10% 5% / 50%)"),
              ParseResultIs(Color(RGBA(51, 26, 13, 127))));

  // Without spacing
  EXPECT_THAT(ColorParser::ParseString("rgb(1%,10%,30%,80%)"),
              ParseResultIs(Color(RGBA(3, 26, 77, 204))));
  EXPECT_THAT(ColorParser::ParseString("rgb(20%10%5%/50%)"),
              ParseResultIs(Color(RGBA(51, 26, 13, 127))));
}

TEST(ColorParser, RgbErrors) {
  EXPECT_THAT(ColorParser::ParseString("rgb(1)"),
              ParseErrorIs("Unexpected EOF when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(1%)"),
              ParseErrorIs("Unexpected EOF when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(invalid)"),
              ParseErrorIs("Unexpected token when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(1 2%)"),
              ParseErrorIs("Unexpected token when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(1 2 3%)"),
              ParseErrorIs("Unexpected token when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(1 2 3/)"),
              ParseErrorIs("Unexpected EOF when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(1 2 / 3)"),
              ParseErrorIs("Unexpected token when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(1 2 3 / 4/)"),
              ParseErrorIs("Additional tokens when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(1,2,3,4,5)"),
              ParseErrorIs("Additional tokens when parsing function 'rgb'"));
  EXPECT_THAT(ColorParser::ParseString("rgb(1 invalid)"),
              ParseErrorIs("Unexpected token when parsing function 'rgb'"));
}

TEST(ColorParser, Hsl) {
  EXPECT_THAT(ColorParser::ParseString("hsl(0 50% 10%)"),
              ParseResultIs(Color(HSLA(0.0f, 0.5f, 0.1f, 255))));
  EXPECT_THAT(ColorParser::ParseString("hsl(  180deg, 50%, 50%  )"),
              ParseResultIs(Color(HSLA(180.0f, 0.5f, 0.5f, 255))));
  EXPECT_THAT(ColorParser::ParseString("hsl(3.14159265359rad 50% 50%)"),
              ParseResultIs(Color(HSLA(180.0f, 0.5f, 0.5f, 255))));

  // hsla is an alias for hsl.
  EXPECT_THAT(ColorParser::ParseString("hsla(180deg 50% 50%)"),
              ParseResultIs(Color(HSLA(180.0f, 0.5f, 0.5f, 255))));

  // Errors if commas are inconsistent.
  EXPECT_THAT(ColorParser::ParseString("hsl(3deg 4%, 5%)"),
              ParseErrorIs("Unexpected token when parsing function 'hsl'"));
  EXPECT_THAT(ColorParser::ParseString("hsla(0, 4% 5%)"),
              ParseErrorIs("Missing comma when parsing function 'hsla'"));

  // With alpha.
  EXPECT_THAT(ColorParser::ParseString("hsl(1, 2%, 3%, 0.04)"),
              ParseResultIs(Color(HSLA(1.0f, 0.02f, 0.03f, 10))));
  EXPECT_THAT(ColorParser::ParseString("hsla(5grad 6% 7% / 8%)"),
              ParseResultIs(Color(HSLA(4.5f, 0.06f, 0.07f, 20))));

  // Invalid alpha.
  EXPECT_THAT(ColorParser::ParseString("hsla(5grad 6% 7% / 30mm)"),
              ParseErrorIs("Unexpected alpha value"));

  // Without spacing.
  EXPECT_THAT(ColorParser::ParseString("hsl(1deg,2%,3%,0.04)"),
              ParseResultIs(Color(HSLA(1.0f, 0.02f, 0.03f, 10))));
  // Space after 'deg' is required to separate the token.
  EXPECT_THAT(ColorParser::ParseString("hsla(5deg 6%7%/8%)"),
              ParseResultIs(Color(HSLA(5.0f, 0.06f, 0.07f, 20))));
}

TEST(ColorParser, HslHues) {
  // All units.
  EXPECT_THAT(ColorParser::ParseString("hsl(0 50% 10%)"),
              ParseResultIs(Color(HSLA(0.0f, 0.5f, 0.1f, 255))));
  EXPECT_THAT(ColorParser::ParseString("hsl(270deg 60% 50%)"),
              ParseResultIs(Color(HSLA(270.0f, 0.6f, 0.5f, 255))));
  EXPECT_THAT(ColorParser::ParseString("hsla(800grad 40% 30%)"),
              ParseResultIs(Color(HSLA(0.0f, 0.4f, 0.3f, 255))));
  EXPECT_THAT(ColorParser::ParseString("hsla(0.9turn 30% 80%)"),
              ParseResultIs(Color(HSLA(324.0f, 0.3f, 0.8f, 255))));

  // Invalid hues.
  EXPECT_THAT(ColorParser::ParseString("hsl(invalid)"),
              ParseErrorIs("Unexpected token when parsing angle"));
  EXPECT_THAT(ColorParser::ParseString("hsl(5in)"),
              ParseErrorIs("Angle has unexpected dimension 'in'"));
  EXPECT_THAT(ColorParser::ParseString("hsl({})"),
              ParseErrorIs("Unexpected token when parsing function 'hsl'"));
}

TEST(ColorParser, HslErrors) {
  EXPECT_THAT(ColorParser::ParseString("hsl(1)"),
              ParseErrorIs("Unexpected EOF when parsing function 'hsl'"));
  EXPECT_THAT(ColorParser::ParseString("hsla(1turn)"),
              ParseErrorIs("Unexpected EOF when parsing function 'hsla'"));
  EXPECT_THAT(ColorParser::ParseString("hsl(1 2)"),
              ParseErrorIs("Unexpected token when parsing function 'hsl'"));
  EXPECT_THAT(ColorParser::ParseString("hsla(1 2% 3)"),
              ParseErrorIs("Unexpected token when parsing function 'hsla'"));
  EXPECT_THAT(ColorParser::ParseString("hsl(1 2% 3%/)"),
              ParseErrorIs("Unexpected EOF when parsing function 'hsl'"));
  EXPECT_THAT(ColorParser::ParseString("hsl(1 2 / 3)"),
              ParseErrorIs("Unexpected token when parsing function 'hsl'"));
  EXPECT_THAT(ColorParser::ParseString("hsla(1 2% 3% / 4/)"),
              ParseErrorIs("Additional tokens when parsing function 'hsla'"));
  EXPECT_THAT(ColorParser::ParseString("hsl(1,2%,3%,4,5)"),
              ParseErrorIs("Additional tokens when parsing function 'hsl'"));
  EXPECT_THAT(ColorParser::ParseString("hsl(1 invalid)"),
              ParseErrorIs("Unexpected token when parsing function 'hsl'"));
}

TEST(ColorParser, Hwb) {
  // Basic HWB color parsing
  EXPECT_THAT(ColorParser::ParseString("hwb(0 0% 0%)"), ParseResultIs(Color(RGBA(255, 0, 0, 255))));
  EXPECT_THAT(ColorParser::ParseString("hwb(120 0% 0%)"),
              ParseResultIs(Color(RGBA(0, 255, 0, 255))));
  EXPECT_THAT(ColorParser::ParseString("hwb(240 0% 0%)"),
              ParseResultIs(Color(RGBA(0, 0, 255, 255))));

  // HWB with alpha
  EXPECT_THAT(ColorParser::ParseString("hwb(0 0% 0% / 0.5)"),
              ParseResultIs(Color(RGBA(255, 0, 0, 128))));
  EXPECT_THAT(ColorParser::ParseString("hwb(120 0% 0% / 25%)"),
              ParseResultIs(Color(RGBA(0, 255, 0, 64))));

  // HWB with percentages
  EXPECT_THAT(ColorParser::ParseString("hwb(0 50% 50%)"), ParseResultIs(Color(RGBA(1, 1, 1, 255))));
  EXPECT_THAT(ColorParser::ParseString("hwb(240 30% 30% / 80%)"),
              ParseResultIs(Color(RGBA(109, 217, 38, 204))));

  // Errors
  EXPECT_THAT(ColorParser::ParseString("hwb(0 0% 0% / invalid)"),
              ParseErrorIs("Unexpected alpha value"));
  EXPECT_THAT(ColorParser::ParseString("hwb(0 0% 0% /)"),
              ParseErrorIs("Unexpected EOF when parsing function 'hwb'"));
  EXPECT_THAT(ColorParser::ParseString("hwb(0 0% 0% 0% 0%)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'hwb'"));
}

TEST(ColorParser, Lab) {
  // Valid lab() parsing
  // Mid gray (L=50%, a=0, b=0)
  EXPECT_THAT(ColorParser::ParseString("lab(50% 0 0)"),
              ParseResultIs(Color(RGBA(119, 119, 119, 255))));
  // White (L=100%)
  EXPECT_THAT(ColorParser::ParseString("lab(100% 0 0)"),
              ParseResultIs(Color(RGBA(255, 255, 255, 255))));
  // Black (L=0%)
  EXPECT_THAT(ColorParser::ParseString("lab(0% 0 0)"), ParseResultIs(Color(RGBA(0, 0, 0, 255))));
  // Red color
  EXPECT_THAT(ColorParser::ParseString("lab(54.29% 80.81 69.89)"),
              ParseResultIs(Color(RGBA(255, 0, 0, 255))));
  // With alpha value
  EXPECT_THAT(ColorParser::ParseString("lab(50% 0 0 / 0.5)"),
              ParseResultIs(Color(RGBA(119, 119, 119, 128))));
  EXPECT_THAT(ColorParser::ParseString("lab(50% 0 0 / 50%)"),
              ParseResultIs(Color(RGBA(119, 119, 119, 127))));
  // Percentages for a and b
  EXPECT_THAT(ColorParser::ParseString("lab(50% 20% -40%)"),
              ParseResultIs(Color(RGBA(122, 106, 205, 255))));
  // Clamping L below 0%
  EXPECT_THAT(ColorParser::ParseString("lab(-10% 0 0)"), ParseResultIs(Color(RGBA(0, 0, 0, 255))));
  // Clamping L above 100%
  EXPECT_THAT(ColorParser::ParseString("lab(110% 0 0)"),
              ParseResultIs(Color(RGBA(255, 255, 255, 255))));
}

TEST(ColorParser, LabErrors) {
  // Missing components
  EXPECT_THAT(ColorParser::ParseString("lab(50% 0)"),
              ParseErrorIs("Unexpected EOF when parsing function 'lab'"));
  // Extra components
  EXPECT_THAT(ColorParser::ParseString("lab(50% 0 0 0)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'lab'"));
  // Invalid tokens
  EXPECT_THAT(ColorParser::ParseString("lab(50% 0 0 invalid)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'lab'"));
  // Missing slash before alpha
  EXPECT_THAT(ColorParser::ParseString("lab(50% 0 0 0.5)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'lab'"));
  // Invalid alpha value
  EXPECT_THAT(ColorParser::ParseString("lab(50% 0 0 / invalid)"),
              ParseErrorIs("Unexpected alpha value"));
}

TEST(ColorParser, Lch) {
  // Valid lch() parsing
  // Mid gray (L=50%, C=0)
  EXPECT_THAT(ColorParser::ParseString("lch(50% 0 0)"),
              ParseResultIs(Color(RGBA(119, 119, 119, 255))));
  // Red color
  EXPECT_THAT(ColorParser::ParseString("lch(54.29% 106.84 40.86)"),
              ParseResultIs(Color(RGBA(255, 0, 0, 255))));
  // With alpha value
  EXPECT_THAT(ColorParser::ParseString("lch(50% 0 0 / 0.5)"),
              ParseResultIs(Color(RGBA(119, 119, 119, 128))));
  EXPECT_THAT(ColorParser::ParseString("lch(50% 0 0 / 50%)"),
              ParseResultIs(Color(RGBA(119, 119, 119, 127))));
  // Clamping L below 0%
  EXPECT_THAT(ColorParser::ParseString("lch(-10% 0 0)"), ParseResultIs(Color(RGBA(0, 0, 0, 255))));
  // Clamping L above 100%
  EXPECT_THAT(ColorParser::ParseString("lch(110% 0 0)"),
              ParseResultIs(Color(RGBA(255, 255, 255, 255))));
  // Negative chroma clamped to 0
  EXPECT_THAT(ColorParser::ParseString("lch(50% -10 30)"),
              ParseResultIs(Color(RGBA(119, 119, 119, 255))));
  // Hue angle normalization
  EXPECT_THAT(ColorParser::ParseString("lch(50% 50 -30deg)"),
              ParseResultIs(Color(RGBA(173, 87, 163, 255))));
}

TEST(ColorParser, LchErrors) {
  // Missing components
  EXPECT_THAT(ColorParser::ParseString("lch(50% 0)"),
              ParseErrorIs("Unexpected EOF when parsing function 'lch'"));
  // Extra components
  EXPECT_THAT(ColorParser::ParseString("lch(50% 0 0 0)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'lch'"));
  // Invalid tokens
  EXPECT_THAT(ColorParser::ParseString("lch(50% 0 0 invalid)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'lch'"));
  // Missing slash before alpha
  EXPECT_THAT(ColorParser::ParseString("lch(50% 0 0 0.5)"),
              ParseErrorIs("Missing delimiter for alpha when parsing function 'lch'"));
  // Invalid alpha value
  EXPECT_THAT(ColorParser::ParseString("lch(50% 0 0 / invalid)"),
              ParseErrorIs("Unexpected alpha value"));
}

}  // namespace donner::css::parser
