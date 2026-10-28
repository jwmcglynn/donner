#include "donner/css/parser/StylesheetParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/tests/TokenTestUtils.h"
#include "donner/css/tests/SelectorTestUtils.h"

using testing::_;
using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::Field;
using testing::IsEmpty;
using testing::Pointee;
using testing::ResultOf;
using testing::VariantWith;

namespace donner::css::parser {

MATCHER_P2(SelectorRuleIs, selector, declarations, "") {
  return testing::ExplainMatchResult(selector, arg.selector, result_listener) &&
         testing::ExplainMatchResult(declarations, arg.declarations, result_listener);
}

std::string SourceText(std::string_view source, SourceRange range) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value() ||
      *range.end.offset < *range.start.offset || *range.end.offset > source.size()) {
    return "<invalid source range>";
  }

  return std::string(source.substr(*range.start.offset, *range.end.offset - *range.start.offset));
}

auto SourceRangeTextIs(std::string_view source, std::string_view expectedText) {
  return ResultOf(
      "source text", [source](SourceRange range) { return SourceText(source, range); },
      Eq(std::string(expectedText)));
}

template <typename SourcesMatcher>
auto FontFaceIs(std::string_view familyName, SourcesMatcher sourcesMatcher) {
  return AllOf(Field("familyName", &FontFace::familyName, Eq(RcString(familyName))),
               Field("sources", &FontFace::sources, sourcesMatcher));
}

auto LocalFontSourceIs(std::string_view familyName) {
  return AllOf(
      Field("kind", &FontFaceSource::kind, FontFaceSource::Kind::Local),
      Field("payload", &FontFaceSource::payload, VariantWith<RcString>(Eq(RcString(familyName)))),
      Field("formatHint", &FontFaceSource::formatHint, Eq(RcString())),
      Field("techHints", &FontFaceSource::techHints, IsEmpty()));
}

template <typename TechHintsMatcher>
auto UrlFontSourceIs(std::string_view url, std::string_view formatHint,
                     TechHintsMatcher techHintsMatcher) {
  return AllOf(Field("kind", &FontFaceSource::kind, FontFaceSource::Kind::Url),
               Field("payload", &FontFaceSource::payload, VariantWith<RcString>(Eq(RcString(url)))),
               Field("formatHint", &FontFaceSource::formatHint, Eq(RcString(formatHint))),
               Field("techHints", &FontFaceSource::techHints, techHintsMatcher));
}

auto UrlFontSourceIs(std::string_view url) {
  return UrlFontSourceIs(url, "", IsEmpty());
}

template <typename BytesMatcher>
auto DataFontSourceIs(std::string_view formatHint, BytesMatcher bytesMatcher) {
  return AllOf(
      Field("kind", &FontFaceSource::kind, FontFaceSource::Kind::Data),
      Field("payload", &FontFaceSource::payload,
            VariantWith<std::shared_ptr<const std::vector<uint8_t>>>(Pointee(bytesMatcher))),
      Field("formatHint", &FontFaceSource::formatHint, Eq(RcString(formatHint))),
      Field("techHints", &FontFaceSource::techHints, IsEmpty()));
}

TEST(StylesheetParser, Empty) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  EXPECT_THAT(StylesheetParser::Parse("", disabled).rules(), ElementsAre());
}

TEST(StylesheetParser, ImportIsUnsupportedButFollowingRulesRemain) {
  constexpr std::string_view kCss = "  @import 'hidden.css'; rect { fill: green; }";
  ParseWarningSink warnings;
  const Stylesheet sheet = StylesheetParser::Parse(kCss, warnings);
  EXPECT_THAT(sheet.rules(), testing::SizeIs(1));
  EXPECT_THAT(warnings.warnings(),
              ElementsAre(ParseDiagnostic::Warning(
                  "CSS @import is unsupported; imported stylesheets are not loaded",
                  FileOffset::Offset(2))));
}

TEST(StylesheetParser, ImportKeywordIsDecodedAndCaseInsensitive) {
  const std::array<std::string_view, 5> imports = {
      R"(@import "hidden.css";)", R"(@import url("hidden.css");)", R"(@ImPoRt url(hidden.css);)",
      R"(@\69mport "hidden.css";)", R"(@\000069 mport url(hidden.css);)"};
  for (const std::string_view import : imports) {
    SCOPED_TRACE(import);
    const std::string source = "  " + std::string(import) + " rect { fill: green; }";
    ParseWarningSink warnings;
    const Stylesheet sheet = StylesheetParser::Parse(source, warnings);
    EXPECT_THAT(sheet.rules(), testing::SizeIs(1));
    EXPECT_THAT(sheet.fontFaces(), IsEmpty());
    EXPECT_THAT(warnings.warnings(),
                ElementsAre(ParseDiagnostic::Warning(
                    "CSS @import is unsupported; imported stylesheets are not loaded",
                    FileOffset::Offset(2))));
  }
}

TEST(StylesheetParser, ImportAtEofIsDiagnosedWithoutInventingSemicolonRecovery) {
  ParseWarningSink warnings;
  const Stylesheet sheet = StylesheetParser::Parse("@import 'hidden.css'", warnings);
  EXPECT_THAT(sheet.rules(), IsEmpty());
  EXPECT_THAT(warnings.warnings(),
              ElementsAre(ParseDiagnostic::Warning(
                  "CSS @import is unsupported; imported stylesheets are not loaded",
                  FileOffset::Offset(0))));
}

TEST(StylesheetParser, ImportOffsetIncludesRemovedCharsetPrefix) {
  constexpr std::string_view source =
      "@charset \"UTF-8\";  @import 'hidden.css'; rect { fill: green; }";
  ParseWarningSink warnings;
  const Stylesheet sheet = StylesheetParser::Parse(source, warnings);
  EXPECT_THAT(sheet.rules(), testing::SizeIs(1));
  ASSERT_THAT(warnings.warnings(), testing::SizeIs(1));
  EXPECT_THAT(warnings.warnings().front().range.start.offset, Eq(source.find("@import")));
}

TEST(StylesheetParser, ImportWarningsAreCappedWithoutEchoingTargetsOrDroppingLocalRules) {
  std::string source;
  for (std::size_t i = 0; i <= ParseWarningSink::kMaximumWarnings; ++i) {
    source += "@import 'https://example.test/private-target.css';";
  }
  source += "rect { fill: green; }";
  ParseWarningSink warnings;
  const Stylesheet sheet = StylesheetParser::Parse(source, warnings);
  EXPECT_THAT(sheet.rules(), testing::SizeIs(1));
  EXPECT_THAT(sheet.securityStats().rejected, testing::IsFalse());
  EXPECT_THAT(warnings.warnings(), testing::SizeIs(ParseWarningSink::kMaximumWarnings));
  EXPECT_THAT(warnings.resourceLimitExceeded(), testing::IsTrue());
  EXPECT_LE(warnings.warningBytes(), ParseWarningSink::kMaximumWarningBytes);
  EXPECT_THAT(warnings.warnings(),
              testing::Each(testing::Field(
                  &ParseDiagnostic::reason,
                  Eq("CSS @import is unsupported; imported stylesheets are not loaded"))));
}

TEST(StylesheetParser, DroppedImportsStillConsumeTheRuleAdmissionBudget) {
  std::string source;
  for (std::size_t i = 0; i <= RuleParser::kMaximumRules; ++i) {
    source += "@import 'hidden.css';";
  }
  ParseWarningSink warnings;
  const Stylesheet sheet = StylesheetParser::Parse(source, warnings);
  EXPECT_THAT(sheet.securityStats().rejected, testing::IsTrue());
  EXPECT_THAT(warnings.resourceLimitExceeded(), testing::IsTrue());
  EXPECT_THAT(warnings.warnings(), testing::SizeIs(ParseWarningSink::kMaximumWarnings));
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
  EXPECT_THAT(
      sheet.rules(),
      ElementsAre(AllOf(
          Field("ruleSourceRange", &SelectorRule::ruleSourceRange,
                SourceRangeTextIs(
                    kCss, R"(rect.cls, [data-tone="warm"] { fill: url(#paint); stroke: blue; })")),
          Field("selectorSourceRange", &SelectorRule::selectorSourceRange,
                SourceRangeTextIs(kCss, R"(rect.cls, [data-tone="warm"])")),
          Field("selectorEntrySourceRanges", &SelectorRule::selectorEntrySourceRanges,
                ElementsAre(SourceRangeTextIs(kCss, "rect.cls"),
                            SourceRangeTextIs(kCss, R"([data-tone="warm"])"))))));
}

TEST(StylesheetParser, SelectorRangesIgnoreCommasAndBracesInStrings) {
  constexpr std::string_view kCss =
      R"(rect[data-list="a,b"], circle { background: url("}"); /* } */ fill: red; })";
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(kCss, disabled);
  EXPECT_THAT(
      sheet.rules(),
      ElementsAre(AllOf(
          Field("ruleSourceRange", &SelectorRule::ruleSourceRange, SourceRangeTextIs(kCss, kCss)),
          Field("selectorEntrySourceRanges", &SelectorRule::selectorEntrySourceRanges,
                ElementsAre(SourceRangeTextIs(kCss, R"(rect[data-list="a,b"])"),
                            SourceRangeTextIs(kCss, "circle"))))));
}

TEST(StylesheetParser, SelectorRangesHandleCommentsEscapesAndNestedArguments) {
  constexpr std::string_view kCss =
      R"(  rect/*,*/, :is(.a, [data-x="a\",b"]), path:nth-child(2n + 1) { fill: red; })";
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(kCss, disabled);
  EXPECT_THAT(
      sheet.rules(),
      ElementsAre(Field("selectorEntrySourceRanges", &SelectorRule::selectorEntrySourceRanges,
                        ElementsAre(SourceRangeTextIs(kCss, "rect/*,*/"),
                                    SourceRangeTextIs(kCss, R"(:is(.a, [data-x="a\",b"]))"),
                                    SourceRangeTextIs(kCss, "path:nth-child(2n + 1)")))));
}

TEST(StylesheetParser, SelectorRuleSourceRangeUsesStylesheetEndForUnclosedRule) {
  constexpr std::string_view kCss = R"(rect { fill: "}"; /* unfinished)";
  ParseWarningSink disabled = ParseWarningSink::Disabled();

  const Stylesheet sheet = StylesheetParser::Parse(kCss, disabled);

  EXPECT_THAT(sheet.rules(),
              ElementsAre(AllOf(Field("ruleSourceRange", &SelectorRule::ruleSourceRange,
                                      SourceRangeTextIs(kCss, kCss)),
                                Field("selectorSourceRange", &SelectorRule::selectorSourceRange,
                                      SourceRangeTextIs(kCss, "rect")))));
}

TEST(StylesheetParser, InvalidSelectorWarnsAndSkipsRule) {
  ParseWarningSink warningSink;
  const Stylesheet sheet =
      StylesheetParser::Parse(R"(, { color: red; } rect { fill: blue; })", warningSink);

  EXPECT_THAT(sheet.rules(), ElementsAre(_));
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

  EXPECT_THAT(sheet.fontFaces(),
              ElementsAre(FontFaceIs("test", ElementsAre(UrlFontSourceIs("test.woff")))));
}

TEST(StylesheetParser, FontFaceMatchingDescriptors) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: Test; src: url(bold.woff);
      font-weight: bold; font-style: italic; font-stretch: condensed;
    }
    @font-face {
      font-family: Test; src: url(regular.woff);
      font-weight: normal; font-style: normal; font-stretch: normal;
    }
  )",
                                                   disabled);
  ASSERT_THAT(sheet.fontFaces(), testing::SizeIs(2));
  EXPECT_EQ(sheet.fontFaces()[0].fontWeight, 700);
  EXPECT_EQ(sheet.fontFaces()[0].fontStyle, 1);
  EXPECT_EQ(sheet.fontFaces()[0].fontStretch, 3);
  EXPECT_EQ(sheet.fontFaces()[1].fontWeight, 400);
  EXPECT_EQ(sheet.fontFaces()[1].fontStyle, 0);
  EXPECT_EQ(sheet.fontFaces()[1].fontStretch, 5);
  EXPECT_NE(FontFaceIdentityKey(sheet.fontFaces()[0]), FontFaceIdentityKey(sheet.fontFaces()[1]));
}

TEST(StylesheetParser, FontFaceIntegerWeights) {
  for (const int weight : {1, 100, 400, 550, 900, 1000}) {
    SCOPED_TRACE(weight);
    ParseWarningSink disabled = ParseWarningSink::Disabled();
    const Stylesheet sheet = StylesheetParser::Parse(
        "@font-face { font-family: Test; src: url(font.woff); font-weight: " +
            std::to_string(weight) + "; }",
        disabled);
    ASSERT_THAT(sheet.fontFaces(), testing::SizeIs(1));
    EXPECT_EQ(sheet.fontFaces()[0].fontWeight, weight);
  }
}

TEST(StylesheetParser, FontFaceStretchKeywords) {
  constexpr std::array<std::string_view, 9> kWidths = {
      "ultra-condensed", "extra-condensed", "condensed",      "semi-condensed", "normal",
      "semi-expanded",   "expanded",        "extra-expanded", "ultra-expanded"};
  for (std::size_t i = 0; i < kWidths.size(); ++i) {
    SCOPED_TRACE(kWidths[i]);
    ParseWarningSink disabled = ParseWarningSink::Disabled();
    const Stylesheet sheet = StylesheetParser::Parse(
        "@font-face { font-family: Test; src: url(font.woff); font-stretch: " +
            std::string(kWidths[i]) + "; }",
        disabled);
    ASSERT_THAT(sheet.fontFaces(), testing::SizeIs(1));
    EXPECT_EQ(sheet.fontFaces()[0].fontStretch, i + 1);
  }
}

TEST(StylesheetParser, FontFaceDescriptorNamesAndKeywordsAreCaseInsensitive) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face { font-family: Test; src: url(font.woff);
      FONT-WEIGHT: BoLd; FONT-STYLE: ObLiQuE; FONT-STRETCH: ExPaNdEd; }
  )",
                                                   disabled);
  ASSERT_THAT(sheet.fontFaces(), testing::SizeIs(1));
  EXPECT_EQ(sheet.fontFaces()[0].fontWeight, 700);
  EXPECT_EQ(sheet.fontFaces()[0].fontStyle, 2);
  EXPECT_EQ(sheet.fontFaces()[0].fontStretch, 7);
}

TEST(StylesheetParser, FontFaceInvalidDescriptorsPreserveLastValidValue) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face { font-family: Test; src: url(font.woff);
      font-weight: bold; font-weight: 500; font-weight: bolder; font-weight: lighter;
      font-weight: 0; font-weight: -1; font-weight: 1001; font-weight: 1e999;
      font-weight: bold garbage; font-weight: "bold"; font-weight: inherit;
      font-weight: normal !important;
      font-style: italic; font-style: oblique; font-style: upright;
      font-style: italic garbage; font-style: initial; font-style: normal !important;
      font-stretch: condensed; font-stretch: expanded; font-stretch: wider;
      font-stretch: narrower; font-stretch: expanded garbage; font-stretch: unset;
      font-stretch: normal !important;
    }
  )",
                                                   disabled);
  ASSERT_THAT(sheet.fontFaces(), testing::SizeIs(1));
  EXPECT_EQ(sheet.fontFaces()[0].fontWeight, 500);
  EXPECT_EQ(sheet.fontFaces()[0].fontStyle, 2);
  EXPECT_EQ(sheet.fontFaces()[0].fontStretch, 7);
}

TEST(StylesheetParser, FontFaceUnsupportedRangesDoNotUseFirstValue) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  const Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face { font-family: Test; src: url(font.woff);
      font-weight: 700 900; font-style: oblique 10deg 20deg; font-stretch: 75% 125%; }
  )",
                                                   disabled);
  ASSERT_THAT(sheet.fontFaces(), testing::SizeIs(1));
  EXPECT_EQ(sheet.fontFaces()[0].fontWeight, 400);
  EXPECT_EQ(sheet.fontFaces()[0].fontStyle, 0);
  EXPECT_EQ(sheet.fontFaces()[0].fontStretch, 5);
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

  EXPECT_THAT(
      sheet.fontFaces(),
      ElementsAre(FontFaceIs("datafont", ElementsAre(DataFontSourceIs(
                                             "font/woff", ElementsAre('t', 'e', 's', 't'))))));
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

  EXPECT_THAT(
      sheet.fontFaces(),
      ElementsAre(FontFaceIs(
          "Fancy Font", ElementsAre(LocalFontSourceIs("Fancy"), LocalFontSourceIs("Fancy Display"),
                                    UrlFontSourceIs("font.woff2", "woff2",
                                                    ElementsAre(RcString("variations"),
                                                                RcString("color-COLRv1"))),
                                    UrlFontSourceIs("font.otf", "opentype", IsEmpty()),
                                    DataFontSourceIs("woff", ElementsAre('t', 'e', 's', 't'))))));
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

  EXPECT_THAT(sheet.fontFaces(),
              ElementsAre(FontFaceIs("TokenFont", ElementsAre(UrlFontSourceIs("font.svg")))));
}

TEST(StylesheetParser, FontFaceSrcSkipsEmptyEntries) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: WhitespaceUrl;
      src: local(Fallback),,;
    }
  )",
                                             disabled);

  EXPECT_THAT(sheet.fontFaces(),
              ElementsAre(FontFaceIs("WhitespaceUrl", ElementsAre(LocalFontSourceIs("Fallback")))));
}

TEST(StylesheetParser, FontFaceSrcSkipsInvalidDataUrls) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: DataUrlFallback;
      src: local(Fallback), url("data:font/woff;base64,not_base64!");
    }
  )",
                                             disabled);

  EXPECT_THAT(
      sheet.fontFaces(),
      ElementsAre(FontFaceIs("DataUrlFallback", ElementsAre(LocalFontSourceIs("Fallback")))));
}

TEST(StylesheetParser, FontFaceSrcKeepsStringUrlAndIdentFormat) {
  ParseWarningSink disabled = ParseWarningSink::Disabled();
  Stylesheet sheet = StylesheetParser::Parse(R"(
    @font-face {
      font-family: urlfallback;
      src: url("font-string.woff") format(opentype);
    }
  )",
                                             disabled);

  EXPECT_THAT(
      sheet.fontFaces(),
      ElementsAre(FontFaceIs(
          "urlfallback", ElementsAre(UrlFontSourceIs("font-string.woff", "opentype", IsEmpty())))));
}

}  // namespace donner::css::parser
