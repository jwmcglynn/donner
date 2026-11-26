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

TEST(DeclarationListParser, BoundsAggregateInlineComponentValuesAndAtRules) {
  std::string declaration = "unknown:";
  for (std::size_t i = 0; i <= DeclarationListParser::kMaximumComponentValues; ++i) {
    declaration += "0 ";
  }

  DeclarationListParser::SecurityStats declarationStats;
  EXPECT_TRUE(DeclarationListParser::ParseOnlyDeclarations(declaration, &declarationStats).empty());
  EXPECT_TRUE(declarationStats.rejected);
  EXPECT_EQ(declarationStats.componentValues, DeclarationListParser::kMaximumComponentValues);

  std::string atRules;
  for (std::size_t rule = 0; rule < 5; ++rule) {
    atRules += "@unknown ";
    for (std::size_t i = 0; i < 8000; ++i) {
      atRules += "0 ";
    }
    atRules += ";";
  }
  DeclarationListParser::SecurityStats atRuleStats;
  const auto parsedAtRules = DeclarationListParser::Parse(atRules, &atRuleStats);
  EXPECT_THAT(parsedAtRules, testing::Not(testing::IsEmpty()));
  EXPECT_TRUE(atRuleStats.rejected);
  EXPECT_EQ(atRuleStats.componentValues, DeclarationListParser::kMaximumComponentValues);
}

TEST(DeclarationListParser, IncompleteTokensHaveNoEditableValueOrDeclarationRange) {
  for (const std::string_view source :
       {"--x: red /* unfinished", "--x: url(#paint", "--x: \"unfinished", "--x: var(--o, red"}) {
    SCOPED_TRACE(source);
    const auto declarations = DeclarationListParser::ParseOnlyDeclarations(source);
    ASSERT_THAT(declarations, ::testing::SizeIs(1));
    EXPECT_THAT(declarations.front().authoredDeclarationRange(), ::testing::Eq(std::nullopt));
    EXPECT_THAT(declarations.front().authoredValueRange(), ::testing::Eq(std::nullopt));
  }
}

TEST(DeclarationListParser, RecordsEditableNameValuePriorityAndNestedReferenceSpans) {
  constexpr std::string_view text =
      R"CSS( --p\61 int /*type*/: var(--ink, url( #spectrum )) /*keep*/ !important ; --empty: ; )CSS";
  const auto declarations = DeclarationListParser::ParseOnlyDeclarations(text);
  ASSERT_THAT(declarations, ::testing::SizeIs(2));
  const auto spelling = [&](const std::optional<SourceRange>& range) {
    if (!range || !range->start.offset || !range->end.offset) {
      return std::string_view{};
    }
    return text.substr(*range->start.offset, *range->end.offset - *range->start.offset);
  };
  const auto& paint = declarations.front();
  EXPECT_THAT(spelling(paint.authoredNameRange()), ::testing::Eq(R"(--p\61 int)"));
  EXPECT_THAT(spelling(paint.authoredValueRange()), ::testing::Eq("var(--ink, url( #spectrum ))"));
  EXPECT_THAT(spelling(paint.authoredPriorityRange()), ::testing::Eq("!important"));
  EXPECT_THAT(
      spelling(paint.authoredDeclarationRange()),
      ::testing::Eq(R"(--p\61 int /*type*/: var(--ink, url( #spectrum )) /*keep*/ !important)"));
  ASSERT_THAT(paint.values, ::testing::SizeIs(1));
  const auto& fn = std::get<Function>(paint.values.front().value);
  ASSERT_THAT(fn.values, ::testing::Not(::testing::IsEmpty()));
  EXPECT_THAT(spelling(fn.values.front().authoredRange()), ::testing::Eq("--ink"));
  const auto& urlToken = std::get<Token>(fn.values.back().value);
  const auto& url = urlToken.get<Token::Url>();
  EXPECT_THAT(text.substr(*urlToken.offset().offset + url.payloadOffset, url.payloadLength),
              ::testing::Eq("#spectrum"));
  const auto emptyValue = declarations.back().authoredValueRange();
  ASSERT_THAT(emptyValue, ::testing::Optional(::testing::_));
  EXPECT_THAT(emptyValue->start.offset, ::testing::Optional(text.rfind(':') + 1));
  EXPECT_EQ(emptyValue->start, emptyValue->end);
}

}  // namespace donner::css::parser
