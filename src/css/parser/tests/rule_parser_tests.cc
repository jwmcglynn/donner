#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/css/parser/rule_parser.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;

namespace donner {
namespace css {

TEST(RuleParser, Empty) {
  EXPECT_THAT(RuleParser::ParseStylesheet(""), ElementsAre());
  EXPECT_THAT(RuleParser::ParseListOfRules(""), ElementsAre());
  EXPECT_THAT(RuleParser::ParseRule(""), Eq(std::nullopt));

  EXPECT_THAT(RuleParser::ParseStylesheet(" \t\f"), ElementsAre());
  EXPECT_THAT(RuleParser::ParseListOfRules(" \r\n"), ElementsAre());
  EXPECT_THAT(RuleParser::ParseRule(" \n "), Eq(std::nullopt));
}

TEST(RuleParser, ParseRule) {
  EXPECT_THAT(RuleParser::ParseRule("rule{}"),
              Optional(QualifiedRuleIs(ElementsAre(TokenIsIdent("rule")),
                                       SimpleBlockIsCurly(ElementsAre()))));
  EXPECT_THAT(RuleParser::ParseRule(" selector > list { key: value }"),
              Optional(QualifiedRuleIs(
                  ElementsAre(TokenIsIdent("selector"), TokenIsWhitespace(" "), TokenIsDelim('>'),
                              TokenIsWhitespace(" "), TokenIsIdent("list"), TokenIsWhitespace(" ")),
                  SimpleBlockIsCurly(ElementsAre(TokenIsWhitespace(" "), TokenIsIdent("key"),
                                                 TokenIsColon(), TokenIsWhitespace(" "),
                                                 TokenIsIdent("value"), TokenIsWhitespace(" "))))));
}

TEST(RuleParser, ParseRule_AtRule) {
  EXPECT_THAT(RuleParser::ParseRule("@other"), Optional(AtRuleIs("other", ElementsAre())));
  EXPECT_THAT(RuleParser::ParseRule("@charset"), Optional(InvalidRuleType()));
  EXPECT_THAT(RuleParser::ParseRule("@charset \"test\""), Optional(InvalidRuleType()));
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

TEST(RuleParser, ParseStylesheet) {
  EXPECT_THAT(RuleParser::ParseStylesheet("rule{}"),
              ElementsAre(QualifiedRuleIs(ElementsAre(TokenIsIdent("rule")),
                                          SimpleBlockIsCurly(ElementsAre()))));
  EXPECT_THAT(RuleParser::ParseStylesheet(" selector > list { key: value }"),
              ElementsAre(QualifiedRuleIs(
                  ElementsAre(TokenIsIdent("selector"), TokenIsWhitespace(" "), TokenIsDelim('>'),
                              TokenIsWhitespace(" "), TokenIsIdent("list"), TokenIsWhitespace(" ")),
                  SimpleBlockIsCurly(ElementsAre(TokenIsWhitespace(" "), TokenIsIdent("key"),
                                                 TokenIsColon(), TokenIsWhitespace(" "),
                                                 TokenIsIdent("value"), TokenIsWhitespace(" "))))));

  EXPECT_THAT(
      RuleParser::ParseStylesheet("rule{} second {value}"),
      ElementsAre(
          QualifiedRuleIs(ElementsAre(TokenIsIdent("rule")), SimpleBlockIsCurly(ElementsAre())),
          QualifiedRuleIs(ElementsAre(TokenIsIdent("second"), TokenIsWhitespace(" ")),
                          SimpleBlockIsCurly(ElementsAre(TokenIsIdent("value"))))));

  EXPECT_THAT(RuleParser::ParseStylesheet("<!-- test -->"), ElementsAre(InvalidRuleType()));
  EXPECT_THAT(RuleParser::ParseStylesheet("-->"), ElementsAre());
}

}  // namespace css
}  // namespace donner