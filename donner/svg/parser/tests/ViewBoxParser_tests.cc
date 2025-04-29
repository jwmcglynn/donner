#include "donner/svg/parser/ViewBoxParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner::svg::parser {

TEST(ViewBoxParser, Empty) {
  EXPECT_THAT(ViewBoxParser::Parse(""),
              ParseErrorIs("Failed to parse number: Unexpected end of string"));
}

TEST(ViewBoxParser, Valid) {
  EXPECT_THAT(ViewBoxParser::Parse("0 0 1 1"), ParseResultIs(Boxd({0, 0}, {1, 1})));
  EXPECT_THAT(ViewBoxParser::Parse("-100 -95 1 3"), ParseResultIs(Boxd({-100, -95}, {-99, -92})));
  EXPECT_THAT(ViewBoxParser::Parse(".5 1.5 1 2.5"), ParseResultIs(Boxd({0.5, 1.5}, {1.5, 4})));

  // width/height of 0,0 is valid, but should be handled by caller.
  EXPECT_THAT(ViewBoxParser::Parse("0 0 0 0"), ParseResultIs(Boxd({0, 0}, {0, 0})));
}

TEST(ViewBoxParser, Commas) {
  // One comma, with spaces -> OK.
  EXPECT_THAT(ViewBoxParser::Parse("0,0,1,1"), ParseResultIs(Boxd({0, 0}, {1, 1})));
  EXPECT_THAT(ViewBoxParser::Parse("0 , 0,  1  ,1"), ParseResultIs(Boxd({0, 0}, {1, 1})));

  // Two commas -> error.
  EXPECT_THAT(ViewBoxParser::Parse("0,,0 1 1"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
}

TEST(ViewBoxParser, ExtraData) {
  EXPECT_THAT(ViewBoxParser::Parse(" 0 0 1 1"),
              ParseErrorIs("Failed to parse number: Unexpected character"));
  EXPECT_THAT(ViewBoxParser::Parse("0 0 1 1 "), ParseErrorIs("Expected end of string"));
  EXPECT_THAT(ViewBoxParser::Parse("0 0 1 1 more"), ParseErrorIs("Expected end of string"));
}

TEST(ViewBoxParser, InvalidSize) {
  EXPECT_THAT(ViewBoxParser::Parse("0 0 -1 -1"),
              ParseErrorIs("Width and height should be positive"));
}

}  // namespace donner::svg::parser
