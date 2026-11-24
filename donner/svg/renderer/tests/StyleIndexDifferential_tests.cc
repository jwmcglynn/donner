/// @file
/// The rule index only skips rules that cannot match: for every document of the repository's SVG
/// corpora, and for documents built around how the index reads keys, the cascade that tests only
/// the index's candidates computes every style, and reports every matched rule, exactly as the
/// cascade that tests every rule.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/components/style/StyleCascadeMetrics.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/tests/ComputedStyleSnapshot.h"
#include "donner/svg/renderer/tests/SvgCorpus.h"

namespace donner::svg::components {
namespace {

using ::testing::ElementsAreArray;
using tests::CorpusDocument;

/// One rule reported by \ref StyleSystem::collectMatchedStyleRules, without its stylesheet entity,
/// which differs between two parses of one document.
struct ReportedRule {
  std::size_t ruleIndex;
  std::size_t selectorEntryIndex;
  css::Specificity specificity;
  bool isUserAgentStylesheet;
  std::optional<SourceRange> selectorSourceRange;

  bool operator==(const ReportedRule&) const = default;

  friend std::ostream& operator<<(std::ostream& os, const ReportedRule& rule) {
    return os << "rule " << rule.ruleIndex << " entry " << rule.selectorEntryIndex << " "
              << rule.specificity << (rule.isUserAgentStylesheet ? " (user agent)" : "");
  }
};

/// The rules matched against every entity of \p document, entity by entity in walk order.
std::vector<std::vector<ReportedRule>> MatchedRules(SVGDocument& document) {
  Registry& registry = document.registry();
  std::vector<std::vector<ReportedRule>> result;
  for (const PositionedEntity& positioned : PositionedEntities(document)) {
    std::vector<ReportedRule>& rules = result.emplace_back();
    for (const MatchedStyleRule& rule :
         StyleSystem().collectMatchedStyleRules(EntityHandle(registry, positioned.entity))) {
      rules.push_back(ReportedRule{rule.ruleIndex, rule.selectorEntryIndex, rule.specificity,
                                   rule.isUserAgentStylesheet, rule.selectorSourceRange});
    }
  }
  return result;
}

void PrepareFrame(SVGDocument& document) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  Registry& registry = document.registry();
  if (!registry.ctx().contains<RenderingContext>()) {
    registry.ctx().emplace<RenderingContext>(registry);
  }
  registry.ctx().get<RenderingContext>().instantiateRenderTree(false, warnings);
}

const StyleCascadeCounters& Counters(SVGDocument& document) {
  return document.registry().ctx().get<StyleCascadeMetrics>().counters;
}

/// Expect \p indexed, which uses the rule index, to compute what \p unindexed computes.
void ExpectSameCascade(SVGDocument& indexed, SVGDocument& unindexed) {
  EXPECT_THAT(PositionedStyles(indexed), ElementsAreArray(PositionedStyles(unindexed)));
  EXPECT_THAT(MatchedRules(indexed), ElementsAreArray(MatchedRules(unindexed)));
  EXPECT_EQ(Counters(indexed).rulesMatched, Counters(unindexed).rulesMatched);
  EXPECT_EQ(Counters(indexed).declarationsApplied, Counters(unindexed).declarationsApplied);
}

SVGDocument ParseSource(std::string_view source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_THAT(parsed, NoParseError());
  return std::move(parsed.result());
}

/// Prepare a frame of \p indexed and of \p unindexed, two parses of one source, the second
/// testing every rule, and expect the same cascade.
void ExpectIndexedMatchesEveryRule(SVGDocument& indexed, SVGDocument& unindexed) {
  unindexed.registry().ctx().emplace<UnindexedCascadeForTesting>();
  PrepareFrame(indexed);
  PrepareFrame(unindexed);

  if (Counters(unindexed).budgetRejections > 0) {
    // Testing every rule exhausted the style budget; the index tests fewer rules, so it may style
    // further but never less.
    ::testing::Test::RecordProperty("unindexed_budget_rejected", "true");
    EXPECT_GE(Counters(indexed).rulesMatched, Counters(unindexed).rulesMatched);
    return;
  }
  ExpectSameCascade(indexed, unindexed);
}

/// The icon sprite, which is compared in halves (see \ref SpriteHalf).
constexpr std::string_view kSpriteName = "bootstrap_icons";

/// The corpus without the icon sprite.
std::vector<CorpusDocument> CorpusDocumentsExceptTheSprite() {
  std::vector<CorpusDocument> documents = tests::CorpusDocuments();
  std::erase_if(documents,
                [](const CorpusDocument& document) { return document.name == kSpriteName; });
  return documents;
}

class StyleIndexDifferentialTest : public ::testing::TestWithParam<CorpusDocument> {};

TEST_P(StyleIndexDifferentialTest, IndexedCascadeMatchesEveryRuleCascade) {
  ASSERT_THAT(std::filesystem::file_size(GetParam().path), ::testing::Gt(0u)) << GetParam();
  std::optional<SVGDocument> indexed = tests::ParseCorpusDocument(GetParam());
  std::optional<SVGDocument> unindexed = tests::ParseCorpusDocument(GetParam());
  if (!indexed || !unindexed) {
    GTEST_SKIP() << "Does not parse";
  }
  ExpectIndexedMatchesEveryRule(*indexed, *unindexed);
}

INSTANTIATE_TEST_SUITE_P(Corpus, StyleIndexDifferentialTest,
                         ::testing::ValuesIn(CorpusDocumentsExceptTheSprite()),
                         [](const ::testing::TestParamInfo<CorpusDocument>& info) {
                           return info.param.name;
                         });

/// Source of the icon sprite.
std::string SpriteSource() {
  std::vector<CorpusDocument> documents = tests::CorpusDocuments();
  const auto sprite =
      std::find_if(documents.begin(), documents.end(),
                   [](const CorpusDocument& document) { return document.name == kSpriteName; });
  EXPECT_NE(sprite, documents.end());
  if (sprite == documents.end()) {
    return {};
  }
  std::ifstream file(sprite->path, std::ios::binary);
  std::ostringstream read;
  read << file.rdbuf();
  return read.str();
}

/**
 * One half of the icon sprite's `<symbol>` elements under its root `<svg>`. The sprite has no
 * stylesheet of its own and no selector of the user agent sheet relates one symbol to another,
 * so each half cascades its elements exactly as the whole sprite does; together the halves hold
 * every element. Whole, the sprite's two parses and two frames are the slowest case of this test
 * under ASan, about 23 s; halves keep each case near half of that.
 *
 * @param half 0 for the first half of the symbols, 1 for the rest.
 */
std::string SpriteHalf(int half) {
  const std::string source = SpriteSource();

  const std::size_t rootOpenEnd = source.find('>', source.find("<svg")) + 1;
  const std::size_t rootClose = source.rfind("</svg>");
  const std::size_t split = source.find("<symbol", rootOpenEnd + (rootClose - rootOpenEnd) / 2);
  EXPECT_NE(source.find("<symbol", rootOpenEnd), split);
  EXPECT_LT(split, rootClose);
  const std::string root = source.substr(0, rootOpenEnd);
  return half == 0 ? root + source.substr(rootOpenEnd, split - rootOpenEnd) + "</svg>"
                   : root + source.substr(split, rootClose - split) + "</svg>";
}

class StyleIndexSpriteTest : public ::testing::TestWithParam<int> {};

TEST_P(StyleIndexSpriteTest, IndexedCascadeMatchesEveryRuleCascade) {
  const std::string source = SpriteHalf(GetParam());
  ASSERT_THAT(source, ::testing::HasSubstr("<symbol"));
  SVGDocument indexed = ParseSource(source);
  SVGDocument unindexed = ParseSource(source);
  ExpectIndexedMatchesEveryRule(indexed, unindexed);
}

INSTANTIATE_TEST_SUITE_P(Halves, StyleIndexSpriteTest, ::testing::Values(0, 1));

TEST(StyleIndexSprite, HalvesTogetherAreTheWholeSprite) {
  const std::string source = SpriteSource();
  const std::string first = SpriteHalf(0);
  const std::string second = SpriteHalf(1);
  constexpr std::string_view kRootClose = "</svg>";
  const std::size_t rootOpenEnd = first.find('>') + 1;
  ASSERT_GT(first.size(), rootOpenEnd + kRootClose.size());
  ASSERT_GT(second.size(), rootOpenEnd + kRootClose.size());
  // The first half without its closing tag, then the second without its root's opening tag, is
  // the sprite up to its closing tag.
  EXPECT_EQ(first.substr(0, first.size() - kRootClose.size()) + second.substr(rootOpenEnd),
            source.substr(0, source.rfind(kRootClose) + kRootClose.size()));
}

/// A document whose keys the index must read as the matcher does.
struct KeyEdgeCase {
  std::string name;
  /// Content of the `<svg>` root.
  std::string body;
  /// Raw `class` values written through the DOM, by element id, for whitespace that XML text
  /// cannot hold, such as a form feed.
  std::vector<std::pair<std::string, std::string>> classWrites;
  /// Rules the every-rule cascade matches across all elements, user-agent rules included, so a
  /// case cannot pass by matching nothing in both cascades. Counted by the cascade, which unlike
  /// the matched-rule report includes imported rules.
  std::size_t rulesMatched;

  friend std::ostream& operator<<(std::ostream& os, const KeyEdgeCase& value) {
    return os << value.name;
  }
};

class StyleIndexKeyEdgeCaseTest : public ::testing::TestWithParam<KeyEdgeCase> {};

TEST_P(StyleIndexKeyEdgeCaseTest, IndexedCascadeMatchesEveryRuleCascade) {
  const std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">)" +
                             GetParam().body + "</svg>";
  // Interactive processing, so a `data:` import loads.
  const auto parse = [&source] {
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    SVGDocument::Settings settings;
    settings.processingMode = ProcessingMode::DynamicInteractive;
    auto parsed = parser::SVGParser::ParseSVG(source, warnings, parser::SVGParser::Options(),
                                              std::move(settings));
    EXPECT_THAT(parsed, NoParseError());
    return std::move(parsed.result());
  };
  SVGDocument indexed = parse();
  SVGDocument unindexed = parse();
  unindexed.registry().ctx().emplace<UnindexedCascadeForTesting>();
  for (SVGDocument* document : {&indexed, &unindexed}) {
    for (const auto& [id, className] : GetParam().classWrites) {
      std::optional<SVGElement> element = document->querySelector("#" + id);
      ASSERT_TRUE(element.has_value()) << id;
      element->setClassName(className);
    }
    PrepareFrame(*document);
  }
  EXPECT_EQ(Counters(unindexed).rulesMatched, GetParam().rulesMatched);
  ExpectSameCascade(indexed, unindexed);
}

INSTANTIATE_TEST_SUITE_P(
    Keys, StyleIndexKeyEdgeCaseTest,
    ::testing::Values(
        // Attribute keys are stored lowercased and looked up ignoring case, in both directions;
        // the matcher decides which spelling matches.
        KeyEdgeCase{
            "AttributeNameCase",
            "<style>[viewBox] { fill: red } [VIEWBOX] { stroke: red } [data-K] { opacity: 0.5 }"
            " [data-k] { stroke-width: 2 } [fill] { stroke-opacity: 0.5 }</style>"
            R"(<rect data-K="1" FILL="red" width="1" height="1"/>)"
            R"(<rect data-k="1" Fill="red" width="1" height="1"/>)"
            R"(<svg viewBox="0 0 1 1"/><svg viewbox="0 0 1 1"/>)",
            {},
            27},
        // Class lists split at space, tab, line feed, carriage return and form feed, with
        // leading, trailing and repeated separators giving no empty tokens.
        KeyEdgeCase{"ClassListWhitespace",
                    "<style>.a { fill: red } .b { stroke: red } .c { opacity: 0.5 }</style>"
                    R"(<rect class="&#9;a&#10;b&#13;" width="1" height="1"/>)"
                    R"(<rect id="t1"/><rect id="t2"/><rect id="t3"/><rect id="t4"/>)"
                    R"(<rect id="t5"/><rect id="t6"/><rect id="t7"/>)",
                    {{"t1", "\ta"},
                     {"t2", "b\nc"},
                     {"t3", "\rc\r"},
                     {"t4", "a\fb"},
                     {"t5", "  c  "},
                     {"t6", "\t\n\f\r "},
                     {"t7", "a\t\tb\n\nc"}},
                    22},
        // Type keys are stored lowercased and looked up ignoring case, including mixed-case SVG
        // element names and an unknown element.
        KeyEdgeCase{"TypeNameCase",
                    "<style>lineargradient { opacity: 0.5 } CLIPPATH { opacity: 0.5 }"
                    " foreignobject { opacity: 0.5 } RECT { fill: red } Rect { stroke: red }"
                    " STOP { stop-opacity: 0.5 }</style>"
                    R"(<defs><linearGradient id="g"><stop offset="0"/></linearGradient>)"
                    R"(<clipPath id="c"><rect width="1" height="1"/></clipPath></defs>)"
                    R"(<foreignObject width="1" height="1"/><Rect/><rect width="1" height="1"/>)",
                    {},
                    20},
        // A shadow element takes its tag from its own tree entity and its id, classes and
        // attributes from the element it instantiates.
        KeyEdgeCase{"UseShadowTree",
                    "<style>.a { fill: red } [data-k] { stroke: red } rect { opacity: 0.5 }"
                    " #src { stroke-width: 2 } use { stroke-opacity: 0.5 }</style>"
                    R"(<defs><rect id="src" data-K="1" width="1" height="1"/></defs>)"
                    R"(<use href="#src"/>)",
                    {{"src", "\ta\fa"}},
                    13},
        // An imported stylesheet has its own index, read the same way.
        KeyEdgeCase{"ImportedStylesheet",
                    "<style>@import url('data:text/css,"
                    "%5BviewBox%5D%7Bfill:red%7D.a%7Bstroke:red%7DlinearGradient%7Bopacity:.5%7D');"
                    "</style>"
                    R"(<svg viewBox="0 0 1 1"/><rect id="i" width="1" height="1"/>)"
                    R"(<defs><linearGradient/></defs>)",
                    {{"i", "\ta"}},
                    17}),
    [](const ::testing::TestParamInfo<KeyEdgeCase>& info) { return info.param.name; });

TEST(StyleIndexDifferential, StylesheetEditsRebuildTheIndex) {
  // Each edit replaces rules after the index was built: a source keystroke that reparses the
  // stylesheet, then DOM writes of new contents. The new keys must take effect on the next frame.
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
      <style>.a { fill: red } rect { stroke: red }</style>
      <rect id="r" class="b" width="8" height="8"/>
      <circle class="a" r="2"/>
    </svg>)svg";
  SVGDocument indexed = ParseSource(kSource);
  SVGDocument unindexed = ParseSource(kSource);
  unindexed.registry().ctx().emplace<UnindexedCascadeForTesting>();
  PrepareFrame(indexed);
  PrepareFrame(unindexed);
  ExpectSameCascade(indexed, unindexed);
  const auto fillOf = [](SVGDocument& document) {
    return document.querySelector("#r")->getComputedStyle().fill.get();
  };
  const auto solid = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return PaintServer(PaintServer::Solid(css::Color(css::RGBA(r, g, b, 0xFF))));
  };
  EXPECT_THAT(fillOf(indexed), ::testing::Optional(solid(0, 0, 0)));

  // `.a` becomes `.b`, which the rect carries.
  for (SVGDocument* document : {&indexed, &unindexed}) {
    const std::string source(document->source());
    const std::size_t at = source.find(".a {") + 1;
    ASSERT_THAT(document
                    ->applySourceEdit(xml::XMLEditIntent{
                        .range = {FileOffset::Offset(at), FileOffset::Offset(at + 1)},
                        .replacement = "b",
                        .sourceVersion = document->sourceVersion(),
                    })
                    .applied,
                ::testing::Eq(true))
        << "keystroke not applied";
    PrepareFrame(*document);
  }
  ExpectSameCascade(indexed, unindexed);
  EXPECT_THAT(fillOf(indexed), ::testing::Optional(solid(0xFF, 0, 0)));

  for (SVGDocument* document : {&indexed, &unindexed}) {
    document->querySelector("style")->cast<SVGStyleElement>().setContents(
        "#r { fill: blue } circle { stroke: green } [class] { opacity: 0.5 }");
    PrepareFrame(*document);
  }
  ExpectSameCascade(indexed, unindexed);
  EXPECT_THAT(fillOf(indexed), ::testing::Optional(solid(0, 0, 0xFF)));

  // Same number of rules, different keys: the index must be rebuilt, not kept by its size.
  for (SVGDocument* document : {&indexed, &unindexed}) {
    document->querySelector("style")->cast<SVGStyleElement>().setContents(
        "#q { fill: blue } rect { fill: green } [id] { opacity: 0.5 }");
    PrepareFrame(*document);
  }
  ExpectSameCascade(indexed, unindexed);
  EXPECT_THAT(fillOf(indexed), ::testing::Optional(solid(0, 0x80, 0)));
}

}  // namespace
}  // namespace donner::svg::components
