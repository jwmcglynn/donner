#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/declaration_list_parser.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::ElementsAre;

namespace donner {
namespace css {

TEST(DeclarationListParser, Empty) {
  EXPECT_THAT(DeclarationListParser::Parse(""), ElementsAre());
}

TEST(DeclarationListParser, Simple) {
  EXPECT_THAT(DeclarationListParser::Parse("test: test"),
              ElementsAre(DeclarationIs("test", ElementsAre(TokenIsIdent("test")))));

  EXPECT_THAT(DeclarationListParser::Parse(" name: value; "),
              ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")))));
}

TEST(DeclarationListParser, Important) {
  EXPECT_THAT(DeclarationListParser::Parse("name: value !important"),
              ElementsAre(DeclarationIs(
                  "name", ElementsAre(TokenIsIdent("value"), TokenIsWhitespace(" ")), true)));
  EXPECT_THAT(DeclarationListParser::Parse("test: !important value"),
              ElementsAre(DeclarationIs(
                  "test", ElementsAre(TokenIsDelim('!'), TokenIsIdent("important"),
                                      TokenIsWhitespace(" "), TokenIsIdent("value")))));
}

// Declaration: When hit ident, read all tokens until EOF
//

// Invalid, read all component values until next semicolon
// Component value: read block, function, or return token
TEST(DeclarationListParser, InvalidTokens) {
  EXPECT_THAT(DeclarationListParser::Parse("< this should be ignored"),
              ElementsAre(InvalidRuleType()));
  EXPECT_THAT(DeclarationListParser::Parse("< ignored { ; key: value }"),
              ElementsAre(InvalidRuleType()));
  EXPECT_THAT(
      DeclarationListParser::Parse("< ignored { ; key: value }; now: valid"),
      ElementsAre(InvalidRuleType(), DeclarationIs("now", ElementsAre(TokenIsIdent("valid")))));
  EXPECT_THAT(
      DeclarationListParser::Parse("! ok: test; { a: a }; [ b: b ]; ( c: c ); now: valid"),
      ElementsAre(InvalidRuleType(), InvalidRuleType(), InvalidRuleType(), InvalidRuleType(),
                  DeclarationIs("now", ElementsAre(TokenIsIdent("valid")))));
}

TEST(DeclarationListParser, AtRule) {
  EXPECT_THAT(DeclarationListParser::Parse("@atrule"),
              ElementsAre(AtRuleIs("atrule", ElementsAre())));

  EXPECT_THAT(
      DeclarationListParser::Parse("@import url(https://example.com) supports(test)"),
      ElementsAre(AtRuleIs(
          "import", ElementsAre(TokenIsWhitespace(" "), TokenIsUrl("https://example.com"),
                                TokenIsWhitespace(" "),
                                FunctionIs("supports", ElementsAre(TokenIsIdent("test")))))));

  EXPECT_THAT(DeclarationListParser::Parse("@with-block { rule: value }"),
              ElementsAre(AtRuleIs(
                  "with-block", ElementsAre(TokenIsWhitespace(" ")),
                  SimpleBlockIsCurly(ElementsAre(TokenIsWhitespace(" "), TokenIsIdent("rule"),
                                                 TokenIsColon(), TokenIsWhitespace(" "),
                                                 TokenIsIdent("value"), TokenIsWhitespace(" "))))));

  EXPECT_THAT(
      DeclarationListParser::Parse("@test test; @thing {}"),
      ElementsAre(AtRuleIs("test", ElementsAre(TokenIsWhitespace(" "), TokenIsIdent("test"))),
                  AtRuleIs("thing", ElementsAre(TokenIsWhitespace(" ")),
                           SimpleBlockIsCurly(ElementsAre()))));

  EXPECT_THAT(
      DeclarationListParser::Parse("@with-block { rule: value } name: value"),
      ElementsAre(
          AtRuleIs("with-block", ElementsAre(TokenIsWhitespace(" ")),
                   SimpleBlockIsCurly(ElementsAre(TokenIsWhitespace(" "), TokenIsIdent("rule"),
                                                  TokenIsColon(), TokenIsWhitespace(" "),
                                                  TokenIsIdent("value"), TokenIsWhitespace(" ")))),
          DeclarationIs("name", ElementsAre(TokenIsIdent("value")))));
}

TEST(DeclarationListParser, OnlyDeclarations) {
  EXPECT_THAT(DeclarationListParser::ParseOnlyDeclarations("@atrule"), ElementsAre());
  EXPECT_THAT(DeclarationListParser::ParseOnlyDeclarations(
                  "@import url(https://example.com) supports(test)"),
              ElementsAre());
  EXPECT_THAT(DeclarationListParser::ParseOnlyDeclarations("@with-block { rule: value }"),
              ElementsAre());
  EXPECT_THAT(DeclarationListParser::ParseOnlyDeclarations("@test test; @thing {}"), ElementsAre());

  EXPECT_THAT(
      DeclarationListParser::ParseOnlyDeclarations(
          "@with-block { rule: value } name: value; name2: value2 !important"),
      ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value"))),
                  DeclarationIs(
                      "name2", ElementsAre(TokenIsIdent("value2"), TokenIsWhitespace(" ")), true)));
}

}  // namespace css
}  // namespace donner
