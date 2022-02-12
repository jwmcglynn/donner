#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/base/tests/base_test_utils.h"
#include "src/svg/parser/viewbox_parser.h"

namespace donner::svg {

TEST(ViewboxParser, Empty) {
  EXPECT_THAT(ViewboxParser::Parse(""), ParseErrorIs("Failed to parse number: Invalid argument"));
}

TEST(ViewboxParser, Valid) {
  EXPECT_THAT(ViewboxParser::Parse("0 0 1 1"), ParseResultIs(Boxd({0, 0}, {1, 1})));
  EXPECT_THAT(ViewboxParser::Parse("-100 -95 1 3"), ParseResultIs(Boxd({-100, -95}, {-99, -92})));
  EXPECT_THAT(ViewboxParser::Parse(".5 1.5 1 2.5"), ParseResultIs(Boxd({0.5, 1.5}, {1.5, 4})));

  // width/height of 0,0 is valid, but should be handled by caller.
  EXPECT_THAT(ViewboxParser::Parse("0 0 0 0"), ParseResultIs(Boxd({0, 0}, {0, 0})));
}

TEST(ViewboxParser, Commas) {
  // One comma, with spaces -> OK.
  EXPECT_THAT(ViewboxParser::Parse("0,0,1,1"), ParseResultIs(Boxd({0, 0}, {1, 1})));
  EXPECT_THAT(ViewboxParser::Parse("0 , 0,  1  ,1"), ParseResultIs(Boxd({0, 0}, {1, 1})));

  // Two commas -> error.
  EXPECT_THAT(ViewboxParser::Parse("0,,0 1 1"),
              ParseErrorIs("Failed to parse number: Invalid argument"));
}

TEST(ViewboxParser, ExtraData) {
  EXPECT_THAT(ViewboxParser::Parse(" 0 0 1 1"),
              ParseErrorIs("Failed to parse number: Invalid argument"));
  EXPECT_THAT(ViewboxParser::Parse("0 0 1 1 "), ParseErrorIs("Expected end of string"));
  EXPECT_THAT(ViewboxParser::Parse("0 0 1 1 more"), ParseErrorIs("Expected end of string"));
}

TEST(ViewboxParser, InvalidSize) {
  EXPECT_THAT(ViewboxParser::Parse("0 0 -1 -1"),
              ParseErrorIs("Width and height should be positive"));
}

}  // namespace donner::svg
