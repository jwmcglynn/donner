#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/color_parser.h"

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

namespace donner {
namespace css {

void PrintTo(const Color& color, std::ostream* os) {
  *os << "Color(";
  if (color.isCurrentColor()) {
    *os << "currentColor";
  } else {
    const RGBA rgba = color.rgba();
    *os << static_cast<int>(rgba.r) << ", " << static_cast<int>(rgba.g) << ", "
        << static_cast<int>(rgba.b) << ", " << static_cast<int>(rgba.a);
  }
  *os << ")";
}

TEST(Color, ColorPrintTo) {
  using namespace string_literals;

  EXPECT_EQ(testing::PrintToString(Color(RGBA(0x11, 0x22, 0x33, 0x44))), "Color(17, 34, 51, 68)");
  EXPECT_EQ(testing::PrintToString(Color(Color::CurrentColor())), "Color(currentColor)");

  EXPECT_EQ(testing::PrintToString(0xFFFFFF_rgb), "Color(255, 255, 255, 255)");
  EXPECT_EQ(testing::PrintToString(0x000000_rgb), "Color(0, 0, 0, 255)");
  EXPECT_EQ(testing::PrintToString(0x123456_rgb), "Color(18, 52, 86, 255)");

  EXPECT_EQ(testing::PrintToString(0xFFFFFF00_rgba), "Color(255, 255, 255, 0)");
  EXPECT_EQ(testing::PrintToString(0x000000CC_rgba), "Color(0, 0, 0, 204)");
  EXPECT_EQ(testing::PrintToString(0x12345678_rgba), "Color(18, 52, 86, 120)");
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

TEST(ColorParser, Function) {
  EXPECT_THAT(ColorParser::ParseString("rgb(1,2,3)"), ParseErrorIs("Not implemented"));
}

}  // namespace css
}  // namespace donner
