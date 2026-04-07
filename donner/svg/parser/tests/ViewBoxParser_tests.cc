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
  EXPECT_THAT(ViewBoxParser::Parse("0 0 1 1"), ParseResultIs(Box2d({0, 0}, {1, 1})));
  EXPECT_THAT(ViewBoxParser::Parse("-100 -95 1 3"), ParseResultIs(Box2d({-100, -95}, {-99, -92})));
  EXPECT_THAT(ViewBoxParser::Parse(".5 1.5 1 2.5"), ParseResultIs(Box2d({0.5, 1.5}, {1.5, 4})));

  // width/height of 0,0 is valid, but should be handled by caller.
  EXPECT_THAT(ViewBoxParser::Parse("0 0 0 0"), ParseResultIs(Box2d({0, 0}, {0, 0})));
}

TEST(ViewBoxParser, Commas) {
  // One comma, with spaces -> OK.
  EXPECT_THAT(ViewBoxParser::Parse("0,0,1,1"), ParseResultIs(Box2d({0, 0}, {1, 1})));
  EXPECT_THAT(ViewBoxParser::Parse("0 , 0,  1  ,1"), ParseResultIs(Box2d({0, 0}, {1, 1})));

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

// ---------------------------------------------------------------------------
// Range-accuracy tests: verify that error SourceRanges cover the right span.
// ---------------------------------------------------------------------------

TEST(ViewBoxParser, RangeExtraData) {
  // "0 0 1 1 " => after parsing 4 numbers (8 chars consumed), whitespace makes remaining
  // non-empty. The space is at offset 7, cursor after "0 0 1 1" is at 7, then skipCommaWhitespace
  // leaves cursor at 8. Actually "0 0 1 1 " after parsing '1' the remaining is " ", currentRange
  // from the non-whitespace after skip. Let me trace: readNumbers reads "0", skips " ", reads "0",
  // skips " ", reads "1", skips " ", reads "1". Consumed = 7. remaining_ = " ". Then
  // remaining_.empty() is false, err.range = currentRange(0,1) = [7,8).
  EXPECT_THAT(ViewBoxParser::Parse("0 0 1 1 "), ParseErrorRange(7, 8));
  // "0 0 1 1 more" => same cursor position, first extra char at offset 8 => [8,9).
  // Wait: readNumbers reads 4 numbers. "0 0 1 1 more" -> 0, skip " ", 0, skip " ", 1, skip " ",
  // 1. After reading "1", consumed = 7. remaining = " more". remaining_ is not empty.
  // currentRange(0,1) = [7,8).
  EXPECT_THAT(ViewBoxParser::Parse("0 0 1 1 more"), ParseErrorRange(7, 8));
}

}  // namespace donner::svg::parser
