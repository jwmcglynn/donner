#include "donner/css/parser/StylesheetParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/parser/tests/TokenTestUtils.h"
#include "donner/css/tests/SelectorTestUtils.h"

using testing::ElementsAre;

namespace donner::css::parser {

MATCHER_P2(SelectorRuleIs, selector, declarations, "") {
  return testing::ExplainMatchResult(selector, arg.selector, result_listener) &&
         testing::ExplainMatchResult(declarations, arg.declarations, result_listener);
}

TEST(StylesheetParser, Empty) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  EXPECT_THAT(StylesheetParser::Parse("", disabled).rules(), ElementsAre());
}

TEST(StylesheetParser, WithRules) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  EXPECT_THAT(StylesheetParser::Parse(R"(
    test, .class {
      name: value;
    }
  )",
                                      disabled)
                  .rules(),
              ElementsAre(SelectorRuleIs(
                  SelectorsAre(ComplexSelectorIs(EntryIs(TypeSelectorIs("test"))),
                               ComplexSelectorIs(EntryIs(ClassSelectorIs("class")))),
                  ElementsAre(DeclarationIs("name", ElementsAre(TokenIsIdent("value")))))));
}

TEST(StylesheetParser, SelectorRuleSourceRanges) {
  constexpr std::string_view kCss =
      "  rect.cls, [data-tone=\"warm\"] { fill: url(#paint); stroke: blue; }\n";
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(kCss, disabled);
  ASSERT_EQ(sheet.rules().size(), 1u);

  const SelectorRule& rule = sheet.rules()[0];
  ASSERT_TRUE(rule.ruleSourceRange.start.offset.has_value());
  ASSERT_TRUE(rule.ruleSourceRange.end.offset.has_value());
  EXPECT_EQ(kCss.substr(*rule.ruleSourceRange.start.offset,
                        *rule.ruleSourceRange.end.offset - *rule.ruleSourceRange.start.offset),
            R"(rect.cls, [data-tone="warm"] { fill: url(#paint); stroke: blue; })");

  ASSERT_TRUE(rule.selectorSourceRange.start.offset.has_value());
  ASSERT_TRUE(rule.selectorSourceRange.end.offset.has_value());
  EXPECT_EQ(
      kCss.substr(*rule.selectorSourceRange.start.offset,
                  *rule.selectorSourceRange.end.offset - *rule.selectorSourceRange.start.offset),
      R"(rect.cls, [data-tone="warm"])");

  ASSERT_EQ(rule.selectorEntrySourceRanges.size(), 2u);
  EXPECT_EQ(kCss.substr(*rule.selectorEntrySourceRanges[0].start.offset,
                        *rule.selectorEntrySourceRanges[0].end.offset -
                            *rule.selectorEntrySourceRanges[0].start.offset),
            "rect.cls");
  EXPECT_EQ(kCss.substr(*rule.selectorEntrySourceRanges[1].start.offset,
                        *rule.selectorEntrySourceRanges[1].end.offset -
                            *rule.selectorEntrySourceRanges[1].start.offset),
            R"([data-tone="warm"])");
}

TEST(StylesheetParser, FontFace) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: test;
      src: url(test.woff);
    }
    svg { fill: red; }
  )",
                                             disabled);

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].familyName, "test");
  ASSERT_EQ(sheet.fontFaces()[0].sources.size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].sources[0].kind, FontFaceSource::Kind::Url);
}

TEST(StylesheetParser, FontFaceDataUrl) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: datafont;
      src: url(data:font/woff;base64,dGVzdA==);
    }
  )",
                                             disabled);

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].familyName, "datafont");
  ASSERT_EQ(sheet.fontFaces()[0].sources.size(), 1u);
  EXPECT_EQ(sheet.fontFaces()[0].sources[0].kind, FontFaceSource::Kind::Data);
  const auto& dataPtr = std::get<std::shared_ptr<const std::vector<uint8_t>>>(
      sheet.fontFaces()[0].sources[0].payload);
  EXPECT_THAT(*dataPtr, testing::ElementsAre('t', 'e', 's', 't'));
}

}  // namespace donner::css::parser
