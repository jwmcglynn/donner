#include "donner/css/parser/StylesheetParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
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

}  // namespace donner::css::parser
