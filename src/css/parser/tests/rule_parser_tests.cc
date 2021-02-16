#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/css/parser/rule_parser.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::ElementsAre;

namespace donner {
namespace css {

TEST(RuleParser, Empty) {
  EXPECT_THAT(RuleParser::ParseStylesheet(""), ElementsAre());
}

TEST(RuleParser, Charset) {
  EXPECT_THAT(RuleParser::ParseStylesheet("@charset \"4\"; @foo"),
              ElementsAre(AtRuleIs("foo", ElementsAre())));
}

}  // namespace css
}  // namespace donner
