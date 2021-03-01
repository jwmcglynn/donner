#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/css/parser/rule_parser.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::ElementsAre;
using testing::Optional;

namespace donner {
namespace css {

TEST(RuleParser, Empty) {
  EXPECT_THAT(RuleParser::ParseStylesheet(""), ElementsAre());
}

TEST(RuleParser, ParseRule_AtRule) {
  EXPECT_THAT(RuleParser::ParseRule("@other"), Optional(AtRuleIs("other", ElementsAre())));
  EXPECT_THAT(RuleParser::ParseRule("@charset"), Optional(InvalidRuleType()));
}

TEST(RuleParser, Charset) {
  EXPECT_THAT(RuleParser::ParseStylesheet("@charset \"4\"; @foo"),
              ElementsAre(AtRuleIs("foo", ElementsAre())));

  // Charset needs to end with `";`
  EXPECT_THAT(RuleParser::ParseStylesheet("@charset \"abc\" { }"), ElementsAre(InvalidRuleType()));
  EXPECT_THAT(RuleParser::ParseStylesheet("@charset \"123\""), ElementsAre(InvalidRuleType()));
  EXPECT_THAT(RuleParser::ParseStylesheet("@charset \"nonterminated"),
              ElementsAre(InvalidRuleType()));

  // Only valid unicode.
  EXPECT_THAT(RuleParser::ParseStylesheet("@charset \"\x80\";"), ElementsAre(InvalidRuleType()));
}

}  // namespace css
}  // namespace donner
