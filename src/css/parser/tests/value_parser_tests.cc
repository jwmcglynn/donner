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
  EXPECT_THAT(ValueParser::Parse(" \t\f"), ElementsAre(TokenIsWhitespace(" \t\f")));
}

TEST(ValueParser, Basic) {
  EXPECT_THAT(ValueParser::Parse("test"), ElementsAre(TokenIsIdent("test")));
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

}  // namespace css
}  // namespace donner
