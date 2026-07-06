#include "donner/css/parser/DeclarationListParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/css/parser/tests/TokenTestUtils.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::Field;

namespace donner::css::parser {

MATCHER_P2(SourceRangeOffsetsAre, startOffset, endOffset, "") {
  if (!arg.start.offset.has_value() || !arg.end.offset.has_value()) {
    *result_listener << "whose offsets are missing";
    return false;
  }

  *result_listener << "whose offsets are [" << *arg.start.offset << ", " << *arg.end.offset << "]";
  return *arg.start.offset == startOffset && *arg.end.offset == endOffset;
}

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
              ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")), true)));
  EXPECT_THAT(DeclarationListParser::Parse("test: !important value"),
              ElementsAre(DeclarationIs(
                  "test", ElementsAre(TokenIsDelim('!'), TokenIsIdent("important"),
                                      TokenIsWhitespace(" "), TokenIsIdent("value")))));

  EXPECT_THAT(DeclarationListParser::Parse("name: value ! /* */ imporTant"),
              ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")), true)));
  EXPECT_THAT(DeclarationListParser::Parse("name: value!/**/\r\nimportant  "),
              ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")), true)));

  EXPECT_THAT(DeclarationListParser::Parse("name: value !important!  "),
              ElementsAre(DeclarationIs(
                  "name",
                  ElementsAre(TokenIsIdent("value"), TokenIsWhitespace(" "), TokenIsDelim('!'),
                              TokenIsIdent("important"), TokenIsDelim('!')),
                  false)));
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

  EXPECT_THAT(DeclarationListParser::ParseOnlyDeclarations(
                  "@with-block { rule: value } name: value; name2: value2 !important"),
              ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value"))),
                          DeclarationIs("name2", ElementsAre(TokenIsIdent("value2")), true)));
}

TEST(DeclarationListParser, SourceRangeSpansNameToLastValueToken) {
  // Regression for the structured-editing M−1 prerequisite: every parsed
  // Declaration must carry a `sourceRange` spanning the name offset to
  // the start of the last consumed value token. The editor's inline-style
  // surgical patcher uses this to locate a specific declaration inside a
  // `style="..."` attribute and splice a new value into it.
  //
  // Byte offsets below are manually asserted so the test fails loudly if
  // the offset plumbing drifts.

  // Single declaration: `fill: red`
  //                      0  3  6
  const auto fillDecls = DeclarationListParser::ParseOnlyDeclarations("fill: red");
  EXPECT_THAT(fillDecls, ElementsAre(Field("sourceRange", &Declaration::sourceRange,
                                           SourceRangeOffsetsAre(0u, 6u))))
      << "sourceRange.end should point at 'r' in 'red' (the last non-whitespace value)";

  // Two declarations: `fill: red; stroke: blue`
  //                    0          11      19
  const auto twoDecls = DeclarationListParser::ParseOnlyDeclarations("fill: red; stroke: blue");
  EXPECT_THAT(
      twoDecls,
      ElementsAre(
          Field("sourceRange", &Declaration::sourceRange, SourceRangeOffsetsAre(0u, 6u)),
          Field("sourceRange", &Declaration::sourceRange, SourceRangeOffsetsAre(11u, 19u))));

  // With !important: `fill: red !important`
  //                   0    5    10   14
  // The !important tokens are popped off `values`; sourceRange.end should
  // stay at the last *value* token (`red`), not at `!` or `important`.
  const auto importantDecls = DeclarationListParser::ParseOnlyDeclarations("fill: red !important");
  EXPECT_THAT(importantDecls, ElementsAre(AllOf(Field("important", &Declaration::important, true),
                                                Field("sourceRange", &Declaration::sourceRange,
                                                      SourceRangeOffsetsAre(0u, 6u)))))
      << "!important markers must not pull sourceRange.end past the value";

  // Multi-token value: `transform: translate(1, 2)`
  //                     0          11
  const auto fnDecls = DeclarationListParser::ParseOnlyDeclarations("transform: translate(1, 2)");
  EXPECT_THAT(fnDecls, ElementsAre(Field("sourceRange", &Declaration::sourceRange,
                                         SourceRangeOffsetsAre(0u, 11u))))
      << "function value's sourceRange.end is the function's name offset";
}

}  // namespace donner::css::parser
