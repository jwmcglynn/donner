#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/css/parser/tests/token_test_utils.h"
#include "src/css/parser/value_parser.h"

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

namespace donner {
namespace css {

TEST(ValueParser, Empty) {
  EXPECT_THAT(ValueParser::Parse(""), ElementsAre());
  EXPECT_THAT(ValueParser::Parse(" \t\f"), ElementsAre());
}

TEST(ValueParser, Basic) {
  EXPECT_THAT(ValueParser::Parse("test"), ElementsAre(TokenIsIdent("test")));
  EXPECT_THAT(ValueParser::Parse(" test \t"), ElementsAre(TokenIsIdent("test")));

  EXPECT_THAT(ValueParser::Parse("rgb(0,1,2)"),
              ElementsAre(FunctionIs(
                  "rgb", ElementsAre(TokenIsNumber(0, "0", NumberType::Integer), TokenIsComma(),
                                     TokenIsNumber(1, "1", NumberType::Integer), TokenIsComma(),
                                     TokenIsNumber(2, "2", NumberType::Integer)))));
  EXPECT_THAT(ValueParser::Parse("one two"),
              ElementsAre(TokenIsIdent("one"), TokenIsWhitespace(" "), TokenIsIdent("two")));
}

TEST(ValueParser, ImportantNotSupported) {
  EXPECT_THAT(ValueParser::Parse("test !important"),
              ElementsAre(TokenIsIdent("test"), TokenIsWhitespace(" "), TokenIsDelim('!'),
                          TokenIsIdent("important")))
      << "!important should not be considered special here";
}

TEST(ValueParser, SupportsComments) {
  EXPECT_THAT(ValueParser::Parse("/*comment*/red"), ElementsAre(TokenIsIdent("red")));
}

TEST(ValueParser, Selector) {
  // Due to a quirk of CSS error handling, this is valid. If we reach the EOF before we reach an end
  // token when parsing a simple block, it is a parser error but the block is returned.
  // This can be confirmed with Javascript which lets parsing single selectors, for example:
  //
  // ```
  // document.querySelector("div[class=cls").style.color = "red";
  // ```
  EXPECT_THAT(ValueParser::Parse("a[ key |= value "),
              ElementsAre(TokenIsIdent("a"),
                          SimpleBlockIsSquare(ElementsAre(
                              TokenIsWhitespace(" "), TokenIsIdent("key"), TokenIsWhitespace(" "),
                              TokenIsDelim('|'), TokenIsDelim('='), TokenIsWhitespace(" "),
                              TokenIsIdent("value"), TokenIsWhitespace(" ")))));

  // The parsed values match both with and without the closing "]" token.
  EXPECT_THAT(ValueParser::Parse("a[ key |= value ]"),
              ElementsAre(TokenIsIdent("a"),
                          SimpleBlockIsSquare(ElementsAre(
                              TokenIsWhitespace(" "), TokenIsIdent("key"), TokenIsWhitespace(" "),
                              TokenIsDelim('|'), TokenIsDelim('='), TokenIsWhitespace(" "),
                              TokenIsIdent("value"), TokenIsWhitespace(" ")))));
}

}  // namespace css
}  // namespace donner
