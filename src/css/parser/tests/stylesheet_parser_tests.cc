#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/css/parser/stylesheet_parser.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::ElementsAre;

namespace donner {
namespace css {

TEST(StylesheetParser, Empty) {
  EXPECT_THAT(StylesheetParser::Parse("").rules, ElementsAre());
}

TEST(StylesheetParser, Charset) {
  EXPECT_THAT(StylesheetParser::Parse("@charset \"4\"; @foo").rules,
              ElementsAre(AtRuleIs("foo", ElementsAre())));
}

}  // namespace css
}  // namespace donner