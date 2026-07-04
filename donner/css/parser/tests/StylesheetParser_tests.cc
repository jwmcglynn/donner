#include "donner/css/parser/StylesheetParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

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

TEST(StylesheetParser, SelectorRangesIgnoreCommasAndBracesInStrings) {
  constexpr std::string_view kCss =
      R"(rect[data-list="a,b"], circle { background: url("}"); /* } */ fill: red; })";
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(kCss, disabled);
  ASSERT_EQ(sheet.rules().size(), 1u);

  const SelectorRule& rule = sheet.rules()[0];
  ASSERT_TRUE(rule.ruleSourceRange.start.offset.has_value());
  ASSERT_TRUE(rule.ruleSourceRange.end.offset.has_value());
  EXPECT_EQ(kCss.substr(*rule.ruleSourceRange.start.offset,
                        *rule.ruleSourceRange.end.offset - *rule.ruleSourceRange.start.offset),
            kCss);

  ASSERT_EQ(rule.selectorEntrySourceRanges.size(), 2u);
  EXPECT_EQ(kCss.substr(*rule.selectorEntrySourceRanges[0].start.offset,
                        *rule.selectorEntrySourceRanges[0].end.offset -
                            *rule.selectorEntrySourceRanges[0].start.offset),
            R"(rect[data-list="a,b"])");
  EXPECT_EQ(kCss.substr(*rule.selectorEntrySourceRanges[1].start.offset,
                        *rule.selectorEntrySourceRanges[1].end.offset -
                            *rule.selectorEntrySourceRanges[1].start.offset),
            "circle");
}

TEST(StylesheetParser, SelectorRangesHandleCommentsEscapesAndNestedArguments) {
  constexpr std::string_view kCss =
      R"(  rect/*,*/, :is(.a, [data-x="a\",b"]), path:nth-child(2n + 1) { fill: red; })";
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(kCss, disabled);
  ASSERT_EQ(sheet.rules().size(), 1u);

  const SelectorRule& rule = sheet.rules()[0];
  ASSERT_EQ(rule.selectorEntrySourceRanges.size(), 3u);
  EXPECT_EQ(kCss.substr(*rule.selectorEntrySourceRanges[0].start.offset,
                        *rule.selectorEntrySourceRanges[0].end.offset -
                            *rule.selectorEntrySourceRanges[0].start.offset),
            "rect/*,*/");
  EXPECT_EQ(kCss.substr(*rule.selectorEntrySourceRanges[1].start.offset,
                        *rule.selectorEntrySourceRanges[1].end.offset -
                            *rule.selectorEntrySourceRanges[1].start.offset),
            R"(:is(.a, [data-x="a\",b"]))");
  EXPECT_EQ(kCss.substr(*rule.selectorEntrySourceRanges[2].start.offset,
                        *rule.selectorEntrySourceRanges[2].end.offset -
                            *rule.selectorEntrySourceRanges[2].start.offset),
            "path:nth-child(2n + 1)");
}

TEST(StylesheetParser, InvalidSelectorWarnsAndSkipsRule) {
  ParseWarningSink warningSink;
  const Stylesheet sheet =
      StylesheetParser::Parse(R"(, { color: red; } rect { fill: blue; })", warningSink);

  ASSERT_EQ(sheet.rules().size(), 1u);
  EXPECT_THAT(
      warningSink.warnings(),
      testing::Contains(testing::Field(&ParseDiagnostic::reason,
                                       testing::HasSubstr("Unexpected token when parsing compound "
                                                          "selector"))));
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

TEST(StylesheetParser, FontFaceMultipleSourcesAndHints) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: "Fancy Font";
      src: local(Fancy),local("Fancy Display"),url(font.woff2) format("woff2")
           tech(variations "color-COLRv1"),url("font.otf") format(opentype),url(data:font/woff;base64,dGVzdA==)
           format("woff");
    }
  )",
                                             disabled);

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  const FontFace& face = sheet.fontFaces()[0];
  EXPECT_EQ(face.familyName, "Fancy Font");
  ASSERT_EQ(face.sources.size(), 5u);

  EXPECT_EQ(face.sources[0].kind, FontFaceSource::Kind::Local);
  EXPECT_EQ(std::get<RcString>(face.sources[0].payload), "Fancy");
  EXPECT_EQ(face.sources[1].kind, FontFaceSource::Kind::Local);
  EXPECT_EQ(std::get<RcString>(face.sources[1].payload), "Fancy Display");

  EXPECT_EQ(face.sources[2].kind, FontFaceSource::Kind::Url);
  EXPECT_EQ(std::get<RcString>(face.sources[2].payload), "font.woff2");
  EXPECT_EQ(face.sources[2].formatHint, "woff2");
  EXPECT_THAT(face.sources[2].techHints,
              testing::ElementsAre(RcString("variations"), RcString("color-COLRv1")));

  EXPECT_EQ(face.sources[3].kind, FontFaceSource::Kind::Url);
  EXPECT_EQ(std::get<RcString>(face.sources[3].payload), "font.otf");
  EXPECT_EQ(face.sources[3].formatHint, "opentype");

  EXPECT_EQ(face.sources[4].kind, FontFaceSource::Kind::Data);
  EXPECT_EQ(face.sources[4].formatHint, "woff");
  const auto& dataPtr =
      std::get<std::shared_ptr<const std::vector<uint8_t>>>(face.sources[4].payload);
  EXPECT_THAT(*dataPtr, testing::ElementsAre('t', 'e', 's', 't'));
}

TEST(StylesheetParser, FontFaceSkipsIncompleteRules) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face { font-family: MissingSrc; }
    @font-face { src: url(font.woff); }
    @font-face { font-family: BadUrl; src: url(); }
  )",
                                             disabled);

  EXPECT_TRUE(sheet.fontFaces().empty());
}

TEST(StylesheetParser, FontFaceSrcUrlTokenAndSkippedInvalidSources) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: TokenFont;
      src: url(font.svg), local(), url(), url(http://example.test/font.woff) format("woff");
    }
    @font-face {
      src: url(orphan.woff);
    }
    @font-face {
      font-family: NoSource;
    }
  )",
                                             disabled);

  ASSERT_EQ(sheet.fontFaces().size(), 1u);
  const FontFace& face = sheet.fontFaces()[0];
  EXPECT_EQ(face.familyName, "TokenFont");
  ASSERT_EQ(face.sources.size(), 1u);
  EXPECT_EQ(face.sources[0].kind, FontFaceSource::Kind::Url);
  EXPECT_EQ(std::get<RcString>(face.sources[0].payload), "font.svg");
}

}  // namespace donner::css::parser
