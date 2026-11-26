/// @file
/// The rule index and the ancestor filter only skip rules that cannot match: for every document of
/// the repository's SVG corpora, and for documents built around how the index reads keys, the
/// cascade that tests only the index's candidates computes every style, and reports every matched
/// rule, exactly as the cascade that tests every rule, and the cascade with the ancestor filter
/// does exactly the same work, apart from its own counters, as the cascade without it.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/style/CustomPropertyMap.h"
#include "donner/svg/components/style/MatchedPropertiesCache.h"
#include "donner/svg/components/style/StyleCascadeMetrics.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RenderingContext.h"
#include "donner/svg/renderer/tests/ComputedStyleSnapshot.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"
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

void ExpectSameStyleOutputs(SVGDocument& actual, SVGDocument& reference) {
  EXPECT_THAT(PositionedStyles(actual), ElementsAreArray(PositionedStyles(reference)));
  EXPECT_THAT(MatchedRules(actual), ElementsAreArray(MatchedRules(reference)));
}

/// Compare candidate selection where both documents have performed the same edits and passes.
void ExpectSameCascade(SVGDocument& indexed, SVGDocument& unindexed) {
  ExpectSameStyleOutputs(indexed, unindexed);
  EXPECT_EQ(Counters(indexed).rulesMatched, Counters(unindexed).rulesMatched);
  EXPECT_EQ(Counters(indexed).declarationsApplied, Counters(unindexed).declarationsApplied);
}

SVGDocument ParseSource(std::string_view source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_THAT(parsed, NoParseError());
  return std::move(parsed.result());
}

TEST(MatchedPropertiesCache, SharedRecordStaysImmutableWhenOneHolderRestyles) {
  SVGDocument document = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.shared { fill: red; stop-opacity: .25; flood-opacity: .75 }</style>
      <g><rect id="first" class="shared"/><rect id="second" class="shared"/></g>
    </svg>)svg");
  PrepareFrame(document);

  EntityHandle first = document.querySelector("#first")->unsafeEntityHandle();
  EntityHandle second = document.querySelector("#second")->unsafeEntityHandle();
  const auto& firstStyle = first.get<ComputedStyleComponent>();
  const auto& secondStyle = second.get<ComputedStyleComponent>();
  ASSERT_NE(firstStyle.style, nullptr);
  ASSERT_EQ(firstStyle.style, secondStyle.style);
  const std::shared_ptr<const ComputedStyle> held = secondStyle.style;
  const ComputedStyle before = *held;

  document.querySelector("#first")->setStyle("fill: blue");
  PrepareFrame(document);

  EXPECT_EQ(held->properties, before.properties);
  EXPECT_EQ(held->resourceOpacities, before.resourceOpacities);
  EXPECT_NE(first.get<ComputedStyleComponent>().style, second.get<ComputedStyleComponent>().style);
}

TEST(MatchedPropertiesCache, RepeatedIsomorphicSubtreesConvergeAfterColdParents) {
  SVGDocument document = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.parent { color: red } .child { fill: currentColor }</style>
      <g id="p1" class="parent"><rect id="c1" class="child"/></g>
      <g id="p2" class="parent"><rect id="c2" class="child"/></g>
    </svg>)svg");
  PrepareFrame(document);

  const auto styleOf = [&](std::string_view selector) -> const ComputedStyleComponent& {
    return document.querySelector(selector)->unsafeEntityHandle().get<ComputedStyleComponent>();
  };
  ASSERT_EQ(styleOf("#p1").style, styleOf("#p2").style);
  ASSERT_EQ(styleOf("#c1").style, styleOf("#c2").style);
  EXPECT_GE(Counters(document).matchedPropertiesCacheHits, 2u);
}

TEST(MatchedPropertiesCache, FullRestyleClearsEntries) {
  SVGDocument document = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.shared { fill: red }</style>
      <rect id="first" class="shared"/><rect class="shared"/>
    </svg>)svg");
  PrepareFrame(document);
  EntityHandle first = document.querySelector("#first")->unsafeEntityHandle();
  const auto& before = first.get<ComputedStyleComponent>();
  ASSERT_THAT(before.properties(), ::testing::NotNull());
  const PropertyRegistry previous = *before.properties();
  const std::weak_ptr<const ComputedStyle> oldRecord = before.style;

  StyleSystem::discardComputedStylesForFullRestyle(document.registry());
  // With no external holder, the whole-discard operation also releases the cache's reference.
  EXPECT_THAT(oldRecord.expired(), ::testing::Eq(true));
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  StyleSystem().computeAllStyles(document.registry(), warnings,
                                 StyleSystem::FullRestyle::ComputeMissing);

  ASSERT_NE(first.get<ComputedStyleComponent>().style, nullptr);
  EXPECT_TRUE(first.get<ComputedStyleComponent>().properties()->exactlyEquals(previous));
}

TEST(MatchedPropertiesCache, WarningProducingCascadesAreNotReused) {
  SVGDocument document = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.shared { fill: definitely-not-a-paint }</style>
      <rect class="shared"/><rect class="shared"/>
    </svg>)svg");
  PrepareFrame(document);

  EXPECT_EQ(Counters(document).matchedPropertiesCacheHits, 0u);
  EXPECT_GE(Counters(document).matchedPropertiesCacheUncacheable, 2u);
}

TEST(MatchedPropertiesCache, AnimatedElementsAreNotReused) {
  SVGDocument document = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="animated" width="10" height="10">
        <animate attributeName="fill" values="red;blue" dur="1s"/>
      </rect>
    </svg>)svg");
  PrepareFrame(document);

  EXPECT_GE(Counters(document).matchedPropertiesCacheUncacheable, 1u);
}

TEST(MatchedPropertiesCache, ResourceOpacityPriorityIsPartOfTheKey) {
  SVGDocument document = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>#rule-stop { stop-opacity: .5 } #rule-flood { flood-opacity: .5 }</style>
      <defs>
        <linearGradient><stop id="rule-stop"/><stop id="attr-stop" stop-opacity=".5"/></linearGradient>
        <filter><feFlood id="rule-flood"/><feFlood id="attr-flood" flood-opacity=".5"/></filter>
      </defs>
    </svg>)svg");
  PrepareFrame(document);

  const auto styleOf = [&](std::string_view selector) -> const ComputedStyleComponent& {
    return document.querySelector(selector)->unsafeEntityHandle().get<ComputedStyleComponent>();
  };
  const auto& ruleStop = styleOf("#rule-stop");
  const auto& attrStop = styleOf("#attr-stop");
  const auto& ruleFlood = styleOf("#rule-flood");
  const auto& attrFlood = styleOf("#attr-flood");
  ASSERT_NE(ruleStop.resourceOpacities(), nullptr);
  ASSERT_NE(attrStop.resourceOpacities(), nullptr);
  ASSERT_NE(ruleFlood.resourceOpacities(), nullptr);
  ASSERT_NE(attrFlood.resourceOpacities(), nullptr);
  EXPECT_DOUBLE_EQ(ruleStop.resourceOpacities()->stopOpacity,
                   attrStop.resourceOpacities()->stopOpacity);
  EXPECT_NE(ruleStop.resourceOpacities()->stopSpecificity,
            attrStop.resourceOpacities()->stopSpecificity);
  EXPECT_DOUBLE_EQ(ruleFlood.resourceOpacities()->floodOpacity,
                   attrFlood.resourceOpacities()->floodOpacity);
  EXPECT_NE(ruleFlood.resourceOpacities()->floodSpecificity,
            attrFlood.resourceOpacities()->floodSpecificity);
  EXPECT_NE(ruleStop.style, attrStop.style);
  EXPECT_NE(ruleFlood.style, attrFlood.style);
}

void ExpectRewrittenStylesheet(bool afterFrame) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
      <style>.a { fill: red }</style>
      <rect id="first" class="a" width="10" height="10"/>
      <rect id="second" class="a" width="10" height="10"/>
    </svg>)svg";
  const PaintServer red(PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255))));
  const PaintServer blue(PaintServer::Solid(css::Color(css::RGBA(0, 0, 255, 255))));

  for (bool disableCache : {false, true}) {
    SCOPED_TRACE(testing::Message() << "cache disabled: " << disableCache);
    SVGDocument document = ParseSource(kSource);
    if (disableCache) {
      document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
    }
    if (afterFrame) {
      PrepareFrame(document);
    }
    auto first = document.querySelector("#first");
    auto second = document.querySelector("#second");
    auto stylesheet = document.querySelector("style");
    ASSERT_THAT(first, testing::Optional(testing::_));
    ASSERT_THAT(second, testing::Optional(testing::_));
    ASSERT_THAT(stylesheet, testing::Optional(testing::_));
    ASSERT_THAT(first->getComputedStyle().fill.get(), testing::Optional(red));

    stylesheet->cast<SVGStyleElement>().setContents(".a { fill: blue }");
    if (afterFrame) {
      second->setStyle("");
    }
    EXPECT_THAT(second->getComputedStyle().fill.get(), testing::Optional(blue));
    PrepareFrame(document);
    EXPECT_THAT(second->getComputedStyle().fill.get(), testing::Optional(blue));
  }
}

TEST(MatchedPropertiesCache, SetContentsBeforeFirstFrameUsesNewRules) {
  ExpectRewrittenStylesheet(/*afterFrame=*/false);
}

TEST(MatchedPropertiesCache, SetContentsBeforeLocalRestyleUsesNewRules) {
  ExpectRewrittenStylesheet(/*afterFrame=*/true);
}

TEST(MatchedPropertiesCache, StyleTypeChangesWhichRulesApply) {
  SVGDocument document = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>rect { fill: red }</style>
      <rect id="first"/><rect id="second"/><rect id="third"/>
    </svg>)svg");
  const PaintServer red(PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255))));
  const PaintServer black(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0, 255))));
  auto first = document.querySelector("#first");
  auto second = document.querySelector("#second");
  auto third = document.querySelector("#third");
  auto stylesheet = document.querySelector("style");
  ASSERT_THAT(first, testing::Optional(testing::_));
  ASSERT_THAT(second, testing::Optional(testing::_));
  ASSERT_THAT(third, testing::Optional(testing::_));
  ASSERT_THAT(stylesheet, testing::Optional(testing::_));
  auto style = stylesheet->cast<SVGStyleElement>();
  ASSERT_THAT(first->getComputedStyle().fill.get(), testing::Optional(red));
  style.setType("text/plain");
  EXPECT_THAT(second->getComputedStyle().fill.get(), testing::Optional(black));
  style.setType("text/css");
  EXPECT_THAT(third->getComputedStyle().fill.get(), testing::Optional(red));
}

TEST(MatchedPropertiesCache, SiblingFloodOpacitiesStayDistinct) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <filter><feFlood id="low" flood-opacity=".2"/><feFlood id="high" flood-opacity=".8"/></filter>
    </svg>)svg";
  for (bool disableCache : {false, true}) {
    SCOPED_TRACE(testing::Message() << "cache disabled: " << disableCache);
    SVGDocument document = ParseSource(kSource);
    if (disableCache) {
      document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
    }
    PrepareFrame(document);
    const auto opacityOf = [&](std::string_view selector) {
      const auto element = document.querySelector(selector);
      const auto& style = element->unsafeEntityHandle().get<ComputedStyleComponent>();
      return style.resourceOpacities()->floodOpacity;
    };
    EXPECT_THAT(opacityOf("#low"), testing::DoubleEq(0.2));
    EXPECT_THAT(opacityOf("#high"), testing::DoubleEq(0.8));
  }
}

TEST(MatchedPropertiesCache, ExternalUseOccurrencesKeepTheirHostPaintOrigins) {
  constexpr std::string_view kHost =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10">
        <use href="sprites.svg#tile" fill="red"/>
        <use href="sprites.svg#tile" x="10" fill="blue"/>
      </svg>)svg";
  constexpr std::string_view kResource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
        <rect id="tile" width="10" height="10"/>
      </svg>)svg";
  std::optional<tests::ExternalUseDocument> cached =
      tests::MakeExternalUseDocument(kHost, std::string(kResource));
  std::optional<tests::ExternalUseDocument> uncached =
      tests::MakeExternalUseDocument(kHost, std::string(kResource), true);
  ASSERT_THAT(cached, ::testing::Optional(::testing::_));
  ASSERT_THAT(uncached, ::testing::Optional(::testing::_));
  Renderer cachedRenderer;
  Renderer uncachedRenderer;
  cachedRenderer.draw(cached->document);
  uncachedRenderer.draw(uncached->document);
  EXPECT_THAT(PositionedStyles(cached->document),
              ElementsAreArray(PositionedStyles(uncached->document)));

  ASSERT_THAT(*cached->resource, ::testing::NotNull());
  ASSERT_THAT(*uncached->resource, ::testing::NotNull());
  SVGDocument cachedResource = SVGDocument::CreateFromHandle(*cached->resource);
  SVGDocument uncachedResource = SVGDocument::CreateFromHandle(*uncached->resource);
  EXPECT_GT(Counters(cachedResource).matchedPropertiesCacheUncacheable, 0u);
  EXPECT_EQ(Counters(uncachedResource).matchedPropertiesCacheHits, 0u);
  EXPECT_EQ(Counters(uncachedResource).matchedPropertiesCacheMisses, 0u);
  EXPECT_EQ(Counters(uncachedResource).matchedPropertiesCacheUncacheable, 0u);
  EXPECT_EQ(Counters(uncachedResource).declarationParsesSkipped, 0u);

  SVGDocument expected = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="20" height="10">
      <rect width="10" height="10" fill="red"/>
      <rect x="10" width="10" height="10" fill="blue"/>
    </svg>)svg");
  Renderer expectedRenderer;
  expectedRenderer.draw(expected);
  editor::tests::CompareBitmapToBitmap(
      cachedRenderer.takeSnapshot(), expectedRenderer.takeSnapshot(),
      "matched-cache-external-use-host-paint-cached", editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(
      uncachedRenderer.takeSnapshot(), expectedRenderer.takeSnapshot(),
      "matched-cache-external-use-host-paint-uncached", editor::tests::PixelmatchIdentityParams());

  cached->document.querySelector("use")->setStyle("fill: green");
  uncached->document.querySelector("use")->setStyle("fill: green");
  cachedRenderer.draw(cached->document);
  uncachedRenderer.draw(uncached->document);
  SVGDocument editedExpected = ParseSource(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="20" height="10">
      <rect width="10" height="10" fill="green"/>
      <rect x="10" width="10" height="10" fill="blue"/>
    </svg>)svg");
  Renderer editedExpectedRenderer;
  editedExpectedRenderer.draw(editedExpected);
  editor::tests::CompareBitmapToBitmap(cachedRenderer.takeSnapshot(),
                                       editedExpectedRenderer.takeSnapshot(),
                                       "matched-cache-external-use-host-paint-edited-cached",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(uncachedRenderer.takeSnapshot(),
                                       editedExpectedRenderer.takeSnapshot(),
                                       "matched-cache-external-use-host-paint-edited-uncached",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(MatchedPropertiesCache, CachedAndUncachedDeclarationBudgetStopAtTheSamePoint) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>
        .shared { fill: red; stroke: blue; stop-opacity: .25; flood-opacity: .75 }
      </style>
      <defs>
        <linearGradient><stop class="shared"/><stop class="shared"/></linearGradient>
        <filter><feFlood class="shared"/><feFlood class="shared"/></filter>
      </defs>
      <rect class="shared"/><rect class="shared"/>
    </svg>)svg";
  for (std::size_t declarationLimit = 1; declarationLimit <= 32; ++declarationLimit) {
    SCOPED_TRACE(declarationLimit);
    SVGDocument cached = ParseSource(kSource);
    SVGDocument uncached = ParseSource(kSource);
    StyleResourceBudget::Limits limits;
    limits.declarationApplications = declarationLimit;
    cached.registry().ctx().emplace<StyleResourceBudget>(limits);
    uncached.registry().ctx().emplace<StyleResourceBudget>(limits);
    uncached.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();

    PrepareFrame(cached);
    PrepareFrame(uncached);

    ExpectSameCascade(cached, uncached);
    EXPECT_EQ(Counters(cached).elementsCascaded, Counters(uncached).elementsCascaded);
    EXPECT_EQ(Counters(cached).rulesTested, Counters(uncached).rulesTested);
    EXPECT_EQ(Counters(cached).rulesMatched, Counters(uncached).rulesMatched);
    EXPECT_EQ(Counters(cached).declarationsApplied, Counters(uncached).declarationsApplied);
    EXPECT_EQ(Counters(cached).budgetRejections, Counters(uncached).budgetRejections);
  }
}

TEST(MatchedPropertiesCache, RetainedByteLimitRefusesTheSameFirstDistinctLocalRecord) {
  constexpr int kElements = 12;
  const auto dasharray = [](int element) {
    std::string value;
    for (int dash = 0; dash < 64; ++dash) {
      value += std::to_string(element * 64 + dash + 1) + " ";
    }
    return value;
  };
  PropertyRegistry sample;
  ASSERT_TRUE(sample.parseStyle("stroke-dasharray:" + dasharray(0)));
  const std::size_t recordBytes = sample.complexPropertyBytes();
  ASSERT_GT(recordBytes, 0u);

  std::string source = R"svg(<svg xmlns="http://www.w3.org/2000/svg">)svg";
  for (int element = 0; element < kElements; ++element) {
    source += "<rect id=\"e" + std::to_string(element) +
              "\" style=\"stroke-dasharray:" + dasharray(element) + "\"/>";
  }
  source += "</svg>";

  const auto firstRefused = [&](bool disableCache) {
    SVGDocument document = ParseSource(source);
    StyleResourceBudget::Limits limits;
    limits.complexPropertyBytes = recordBytes * 4;
    document.registry().ctx().emplace<StyleResourceBudget>(limits);
    if (disableCache) {
      document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
    }
    PrepareFrame(document);
    for (int element = 0; element < kElements; ++element) {
      const auto& style = document.querySelector("#e" + std::to_string(element))
                              ->unsafeEntityHandle()
                              .get<ComputedStyleComponent>();
      if (style.properties()->strokeDasharray.state == PropertyState::NotSet) {
        return element;
      }
    }
    return kElements;
  };

  const int uncachedFirstRefused = firstRefused(true);
  const int cachedFirstRefused = firstRefused(false);
  EXPECT_LT(uncachedFirstRefused, kElements);
  EXPECT_EQ(cachedFirstRefused, uncachedFirstRefused);
}

TEST(MatchedPropertiesCache, OrderedWarningsMatchAcrossFirstFullAndRetainedPasses) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.bad { fill: definitely-not-a-paint; stroke: also-not-a-paint }</style>
      <rect id="target" class="bad"/><rect class="bad"/>
    </svg>)svg";
  const auto warningSequence = [&](bool disableCache) {
    SVGDocument document = ParseSource(kSource);
    if (disableCache) {
      document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
    }
    std::vector<std::vector<ParseDiagnostic>> result;
    Registry& registry = document.registry();
    registry.ctx().emplace<RenderingContext>(registry);
    {
      ParseWarningSink warnings;
      registry.ctx().get<RenderingContext>().instantiateRenderTree(false, warnings);
      result.push_back(warnings.warnings());
    }
    {
      StyleSystem::discardComputedStylesForFullRestyle(registry);
      ParseWarningSink warnings;
      StyleSystem().computeAllStyles(registry, warnings, StyleSystem::FullRestyle::ComputeMissing);
      result.push_back(warnings.warnings());
    }
    {
      const Entity entity = document.querySelector("#target")->unsafeEntityHandle().entity();
      StyleSystem styleSystem;
      styleSystem.invalidateComputed(EntityHandle(registry, entity));
      ParseWarningSink warnings;
      styleSystem.computeStylesFor(registry, std::span<const Entity>(&entity, 1), warnings);
      result.push_back(warnings.warnings());
    }
    return result;
  };

  const std::vector<std::vector<ParseDiagnostic>> disabledFirst = warningSequence(true);
  const std::vector<std::vector<ParseDiagnostic>> enabledSecond = warningSequence(false);
  ASSERT_EQ(disabledFirst.size(), 3u);
  EXPECT_FALSE(disabledFirst[0].empty());
  EXPECT_THAT(enabledSecond, ElementsAreArray(disabledFirst));
}

struct CascadePassSnapshot {
  std::vector<PositionedStyle> styles;
  std::vector<std::vector<ReportedRule>> rules;
  std::vector<ParseDiagnostic> warnings;
  StyleCascadeCounters counters;
  std::size_t warningAttempts = 0;
  std::size_t warningBytes = 0;
  bool warningLimitExceeded = false;
  bool styleBudgetRejected = false;
};

CascadePassSnapshot CaptureCascadePass(SVGDocument& document, ParseWarningSink& warnings,
                                       const StyleCascadeCounters& before) {
  Registry& registry = document.registry();
  const auto* budget = registry.ctx().find<StyleResourceBudget>();
  return CascadePassSnapshot{
      .styles = PositionedStyles(document),
      .rules = MatchedRules(document),
      .warnings = warnings.warnings(),
      .counters = Counters(document) - before,
      .warningAttempts = warnings.warningAttempts(),
      .warningBytes = warnings.warningBytes(),
      .warningLimitExceeded = warnings.resourceLimitExceeded(),
      .styleBudgetRejected = budget && budget->rejected(),
  };
}

CascadePassSnapshot PrepareFirstFrameWithDiagnostics(SVGDocument& document) {
  Registry& registry = document.registry();
  const StyleCascadeCounters before =
      registry.ctx().contains<StyleCascadeMetrics>() ? Counters(document) : StyleCascadeCounters();
  if (!registry.ctx().contains<RenderingContext>()) {
    registry.ctx().emplace<RenderingContext>(registry);
  }
  ParseWarningSink warnings;
  registry.ctx().get<RenderingContext>().instantiateRenderTree(false, warnings);
  return CaptureCascadePass(document, warnings, before);
}

// Normalize reference targets by tree position, since the two documents own different entities.
auto PositionedRenderInstances(SVGDocument& document) {
  const Registry& registry = document.registry();
  const auto positions = PositionedEntities(document);
  const auto position = [&](Entity entity) -> std::string {
    const auto found = std::ranges::find(positions, entity, &PositionedEntity::entity);
    return found == positions.end() ? std::string() : found->position;
  };
  const auto paintReference = [&](const ResolvedPaintServer& paint) {
    const auto* reference = std::get_if<PaintResolvedReference>(&paint);
    return reference ? position(reference->reference) : std::string();
  };
  const auto referenceTarget = [&](const auto& reference) {
    return reference ? position(reference->reference) : std::string();
  };
  using Instance = std::tuple<std::string, int, bool, bool, std::string, Transform2d,
                              std::array<std::string, 8>>;
  std::vector<Instance> result;
  for (const auto& entity : positions) {
    const auto* instance = registry.try_get<RenderingInstanceComponent>(entity.entity);
    if (!instance) {
      continue;
    }
    const auto* filter = instance->resolvedFilter
                             ? std::get_if<ResolvedReference>(&*instance->resolvedFilter)
                             : nullptr;
    result.emplace_back(
        entity.position, instance->drawOrder, instance->visible, instance->isolatedLayer,
        position(instance->subtreeLastEntity), instance->worldFromEntityTransform,
        std::array{paintReference(instance->resolvedFill), paintReference(instance->resolvedStroke),
                   referenceTarget(instance->clipPath), referenceTarget(instance->mask),
                   referenceTarget(instance->markerStart), referenceTarget(instance->markerMid),
                   referenceTarget(instance->markerEnd),
                   filter ? position(*filter) : std::string()});
  }
  return result;
}

TEST(StyleInvalidationDifferential, PublicEditsMatchFullRestyle) {
  enum class FrameRoute { Retained, FullFrame, FullStyle };
  struct Edit {
    std::string_view name;
    std::string_view contents;
    void (*apply)(SVGElement&);
    FrameRoute route;
    std::optional<std::size_t> affectedCount = std::nullopt;
  };
  const Edit edits[] = {
      {"class-subject",
       R"(<style>.hot { fill: red }</style><rect id="target" width="8" height="8"/>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::Retained, 1},
      {"class-attribute-order",
       R"(<style>[class="cold hot"] { fill: red }</style><rect id="target" class="hot cold" width="8" height="8"/>)",
       [](SVGElement& e) { e.setClassName("cold hot"); }, FrameRoute::Retained, 1},
      {"class-inheritance",
       R"(<style>.hot { fill: red }</style><g id="target"><rect width="8" height="8"/></g>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::Retained, 2},
      {"class-following-sibling",
       R"(<style>.hot + .after rect { fill: red }</style><g><g id="target"/><g class="after"><rect width="8" height="8"/></g></g>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::Retained, 3},
      {"class-filtered-nth-child",
       R"(<style>rect:nth-child(1 of :is(.hot)) { fill: red }</style><g><rect id="target" width="8" height="8"/><rect class="hot" x="8" width="8" height="8"/></g>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::Retained, 2},
      {"class-filtered-nth-last-child",
       R"(<style>rect:nth-last-child(1 of :is(.hot)) { fill: red }</style><g><rect class="hot" width="8" height="8"/><rect id="target" x="8" width="8" height="8"/></g>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::Retained, 2},
      {"style", R"(<rect id="target" width="8" height="8"/>)",
       [](SVGElement& e) { e.setStyle("fill: red"); }, FrameRoute::Retained, 1},
      {"update-style",
       R"(<style>[style] + rect { fill: red }</style><g><g id="target"/><rect width="8" height="8"/></g>)",
       [](SVGElement& e) { e.updateStyle("fill: red"); }, FrameRoute::Retained, 2},
      {"presentation-attribute",
       R"(<style>[fill=red] + rect { fill: lime }</style><g><rect id="target" width="8" height="8"/><rect x="8" width="8" height="8"/></g>)",
       [](SVGElement& e) { e.setAttribute("fill", "red"); }, FrameRoute::Retained, 2},
      {"presentation-removal",
       R"(<style>[fill] + rect { fill: lime }</style><g><rect id="target" fill="red" width="8" height="8"/><rect x="8" width="8" height="8"/></g>)",
       [](SVGElement& e) { e.removeAttribute("fill"); }, FrameRoute::Retained, 2},
      {"ancestor-selector",
       R"(<style>g:has(.hot) { fill: red }</style><g><rect id="target" width="8" height="8"/></g>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::FullStyle},
      {"resource-subtree",
       R"svg(<defs><linearGradient id="paint"><stop id="target" stop-color="blue"/></linearGradient></defs><rect fill="url(#paint)" width="8" height="8"/>)svg",
       [](SVGElement& e) { e.setAttribute("stop-color", "red"); }, FrameRoute::FullStyle},
      {"remove-paint-reference",
       R"svg(<defs><linearGradient id="paint"><stop stop-color="blue"/></linearGradient></defs><rect id="target" fill="url(#paint)" width="8" height="8"/>)svg",
       [](SVGElement& e) { e.setAttribute("fill", "red"); }, FrameRoute::FullFrame, 1},
      {"id-reference",
       R"(<rect id="target" fill="red" width="8" height="8"/><use href="#target" x="8"/>)",
       [](SVGElement& e) { e.setId("other"); }, FrameRoute::FullStyle},
      {"uninstantiated-symbol",
       R"(<style>.hot { fill: red }</style><symbol id="tile"><path id="target" d="M0 0H8V8H0Z"/></symbol>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::FullFrame, 1},
      {"instantiated-symbol",
       R"(<style>.hot { fill: red }</style><symbol id="tile"><path id="target" d="M0 0H8V8H0Z"/></symbol><use href="#tile" width="8" height="8"/>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::FullFrame, 3},
      {"use-source",
       R"(<style>.hot { fill: red }</style><g id="target"><rect width="8" height="8"/></g><use href="#target" x="8"/>)",
       [](SVGElement& e) { e.setClassName("hot"); }, FrameRoute::Retained, 4},
  };
  for (const auto& edit : edits) {
    SCOPED_TRACE(edit.name);
    const std::string source =
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">)") +
        std::string(edit.contents) + R"(<circle cx="12" cy="12" r="2"/></svg>)";
    SVGDocument narrow = ParseSource(source);
    SVGDocument full = ParseSource(source);
    const auto beforeEdit = PrepareFirstFrameWithDiagnostics(narrow);
    std::ignore = PrepareFirstFrameWithDiagnostics(full);
    for (SVGDocument* document : {&narrow, &full}) {
      auto target = document->querySelector("#target");
      ASSERT_THAT(target, ::testing::Optional(::testing::_));
      edit.apply(*target);
    }
    auto& narrowState = narrow.registry().ctx().get<RenderTreeState>();
    auto& fullState = full.registry().ctx().get<RenderTreeState>();
    const bool expectFullStyle = edit.route == FrameRoute::FullStyle;
    const bool expectRetained = edit.route == FrameRoute::Retained;
    EXPECT_EQ(narrowState.needsFullStyleRecompute, expectFullStyle);
    fullState.retainedUpdatesEnabled = false;
    fullState.needsFullStyleRecompute = true;
    const auto before = narrowState.counters;
    const auto referenceBefore = fullState.counters;
    const auto narrowPass = PrepareFirstFrameWithDiagnostics(narrow);
    const auto fullPass = PrepareFirstFrameWithDiagnostics(full);

    EXPECT_THAT(narrowPass.styles, ElementsAreArray(fullPass.styles));
    EXPECT_THAT(narrowPass.styles, ::testing::Not(ElementsAreArray(beforeEdit.styles)));
    EXPECT_THAT(narrowPass.rules, ElementsAreArray(fullPass.rules));
    EXPECT_THAT(narrowPass.warnings, ElementsAreArray(fullPass.warnings));
    EXPECT_THAT(PositionedRenderInstances(narrow),
                ElementsAreArray(PositionedRenderInstances(full)));
    EXPECT_EQ(narrowState.counters.fullRebuilds - before.fullRebuilds, expectRetained ? 0u : 1u);
    EXPECT_EQ(narrowState.counters.retainedUpdates - before.retainedUpdates,
              expectRetained ? 1u : 0u);
    EXPECT_EQ(fullState.counters.fullRebuilds - referenceBefore.fullRebuilds, 1u);
    EXPECT_EQ(fullState.counters.retainedUpdates - referenceBefore.retainedUpdates, 0u);
    EXPECT_EQ(narrowPass.counters.fullClears, expectFullStyle ? 1u : 0u);
    EXPECT_EQ(fullPass.counters.fullClears, 1u);
    if (edit.affectedCount) {
      EXPECT_EQ(narrowPass.counters.elementsCascaded, *edit.affectedCount);
      EXPECT_GT(fullPass.counters.elementsCascaded, narrowPass.counters.elementsCascaded);
    }

    Renderer narrowRenderer;
    Renderer fullRenderer;
    narrowRenderer.draw(narrow);
    fullRenderer.draw(full);
    editor::tests::CompareBitmapToBitmap(narrowRenderer.takeSnapshot(), fullRenderer.takeSnapshot(),
                                         std::string(edit.name),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(StyleInvalidationDifferential, LaterUseReadsPreviouslyRestyledSymbol) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>.hot { fill: red }</style>
      <symbol id="tile"><path id="target" d="M0 0H8V8H0Z"/></symbol>
      <use id="consumer" href="#missing" width="8" height="8"/>
    </svg>)svg";
  SVGDocument narrow = ParseSource(kSource);
  SVGDocument full = ParseSource(kSource);
  for (SVGDocument* document : {&narrow, &full}) {
    std::ignore = PrepareFirstFrameWithDiagnostics(*document);
    auto target = document->querySelector("#target");
    ASSERT_THAT(target, ::testing::Optional(::testing::_));
    target->setClassName("hot");
  }
  EXPECT_EQ(PrepareFirstFrameWithDiagnostics(narrow).counters.elementsCascaded, 1u);
  full.registry().ctx().get<RenderTreeState>().needsFullStyleRecompute = true;
  std::ignore = PrepareFirstFrameWithDiagnostics(full);

  for (SVGDocument* document : {&narrow, &full}) {
    auto consumer = document->querySelector("#consumer");
    ASSERT_THAT(consumer, ::testing::Optional(::testing::_));
    consumer->setAttribute("href", "#tile");
  }
  full.registry().ctx().get<RenderTreeState>().needsFullStyleRecompute = true;
  const auto narrowPass = PrepareFirstFrameWithDiagnostics(narrow);
  const auto fullPass = PrepareFirstFrameWithDiagnostics(full);
  EXPECT_THAT(narrowPass.styles, ElementsAreArray(fullPass.styles));
  EXPECT_THAT(PositionedRenderInstances(narrow), ElementsAreArray(PositionedRenderInstances(full)));
  Renderer narrowRenderer;
  Renderer fullRenderer;
  narrowRenderer.draw(narrow);
  fullRenderer.draw(full);
  editor::tests::CompareBitmapToBitmap(narrowRenderer.takeSnapshot(), fullRenderer.takeSnapshot(),
                                       "later-use", editor::tests::PixelmatchIdentityParams());
}

TEST(StyleInvalidationDifferential, LimitedPassPreservesNewRefusalAndWarnings) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>
        .hot { fill: not-a-color; }
        .hot { stroke: red; }
        .distal { fill: lime; }
      </style>
      <rect class="distal" id="distal" width="8" height="8"/>
      <rect id="target" x="8" width="8" height="8"/>
    </svg>)svg";
  SVGDocument measurement = ParseSource(kSource);
  const auto base = PrepareFirstFrameWithDiagnostics(measurement);
  for (bool limitAfterEdit : {false, true}) {
    SCOPED_TRACE(::testing::Message() << "limit installed after routing=" << limitAfterEdit);
    StyleResourceBudget::Limits limits;
    // Adding the class exceeds the previously admitted full workload. With a late limit of
    // two even the selective pass refuses, which exercises the renderer's actual retry.
    limits.ruleElementMatches = limitAfterEdit ? 2u : base.counters.rulesTested;

    SVGDocument narrow = ParseSource(kSource);
    SVGDocument full = ParseSource(kSource);
    for (SVGDocument* document : {&narrow, &full}) {
      auto& context = document->registry().ctx();
      if (!limitAfterEdit) {
        context.emplace<StyleResourceBudget>(limits);
      }
      const auto before = PrepareFirstFrameWithDiagnostics(*document);
      EXPECT_EQ(before.counters.budgetRejections, 0u);
      auto target = document->querySelector("#target");
      ASSERT_THAT(target, ::testing::Optional(::testing::_));
      target->setClassName("hot");
      if (limitAfterEdit) {
        context.erase<StyleResourceBudget>();
        context.emplace<StyleResourceBudget>(limits);
      }
    }

    auto& narrowState = narrow.registry().ctx().get<RenderTreeState>();
    EXPECT_EQ(narrowState.needsFullStyleRecompute, !limitAfterEdit);
    full.registry().ctx().get<RenderTreeState>().needsFullStyleRecompute = true;
    const auto narrowPass = PrepareFirstFrameWithDiagnostics(narrow);
    const auto fullPass = PrepareFirstFrameWithDiagnostics(full);
    EXPECT_GT(fullPass.counters.budgetRejections, 0u);
    EXPECT_GT(fullPass.warningAttempts, 0u);
    EXPECT_EQ(narrowPass.counters.fullClears, 1u);
    EXPECT_EQ(narrowPass.counters.budgetRejections,
              fullPass.counters.budgetRejections + static_cast<std::uint64_t>(limitAfterEdit));
    EXPECT_EQ(narrowPass.warningAttempts, fullPass.warningAttempts);
    EXPECT_THAT(narrowPass.warnings, ElementsAreArray(fullPass.warnings));
    EXPECT_THAT(narrowPass.styles, ElementsAreArray(fullPass.styles));
    EXPECT_THAT(PositionedRenderInstances(narrow),
                ElementsAreArray(PositionedRenderInstances(full)));
  }
}

CascadePassSnapshot ForceFullRestyleWithDiagnostics(SVGDocument& document) {
  Registry& registry = document.registry();
  const StyleCascadeCounters before = Counters(document);
  StyleSystem::discardComputedStylesForFullRestyle(registry);
  ParseWarningSink warnings;
  StyleSystem().computeAllStyles(registry, warnings, StyleSystem::FullRestyle::ComputeMissing);
  return CaptureCascadePass(document, warnings, before);
}

CascadePassSnapshot RecomputeLastPositionedStyleWithDiagnostics(SVGDocument& document) {
  Registry& registry = document.registry();
  const std::vector<PositionedEntity> entities = PositionedEntities(document);
  EXPECT_FALSE(entities.empty());
  if (entities.empty()) {
    return {};
  }
  const Entity entity = entities.back().entity;
  const StyleCascadeCounters before = Counters(document);
  StyleSystem styleSystem;
  styleSystem.invalidateComputed(EntityHandle(registry, entity));
  ParseWarningSink warnings;
  styleSystem.computeStylesFor(registry, std::span<const Entity>(&entity, 1), warnings);
  return CaptureCascadePass(document, warnings, before);
}

void ExpectSameLogicalCounters(const StyleCascadeCounters& cached,
                               const StyleCascadeCounters& disabled) {
  EXPECT_EQ(cached.elementsCascaded, disabled.elementsCascaded);
  EXPECT_EQ(cached.rulesTested, disabled.rulesTested);
  EXPECT_EQ(cached.rulesMatched, disabled.rulesMatched);
  EXPECT_EQ(cached.declarationsApplied, disabled.declarationsApplied);
  EXPECT_EQ(cached.fullClears, disabled.fullClears);
  EXPECT_EQ(cached.budgetRejections, disabled.budgetRejections);
}

void ExpectSameCacheIndependentPass(const CascadePassSnapshot& cached,
                                    const CascadePassSnapshot& disabled) {
  EXPECT_THAT(cached.styles, ElementsAreArray(disabled.styles));
  EXPECT_THAT(cached.rules, ElementsAreArray(disabled.rules));
  EXPECT_THAT(cached.warnings, ElementsAreArray(disabled.warnings));
  ExpectSameLogicalCounters(cached.counters, disabled.counters);
  EXPECT_EQ(cached.warningAttempts, disabled.warningAttempts);
  EXPECT_EQ(cached.warningBytes, disabled.warningBytes);
  EXPECT_EQ(cached.warningLimitExceeded, disabled.warningLimitExceeded);
  EXPECT_EQ(cached.styleBudgetRejected, disabled.styleBudgetRejected);
}

/// \p counters without the ancestor filter's own counters, which only the filtered pass counts.
StyleCascadeCounters WithoutAncestorFilterCounters(StyleCascadeCounters counters) {
  counters.ancestorFilterRejections = 0;
  counters.ancestorFilterFalsePositives = 0;
  counters.ancestorFilterSaturations = 0;
  counters.ancestorFilterKeyLimits = 0;
  return counters;
}

/**
 * Expect the pass of a document with the ancestor filter to equal the same pass without it:
 * styles, matched-rule lists, ordered warnings and every counter but the filter's own, which the
 * unfiltered pass leaves at zero. Skipped candidates are still tested and charged, so even the
 * work counts agree. A pass the unfiltered cascade ended budget-rejected may only style further
 * with the filter, because the walks it skips are not charged to the traversal budget.
 *
 * The matched-rule lists come from \ref StyleSystem::collectMatchedStyleRules, which runs no
 * pass and so never consults the filter; the cascade's own matches are compared through the
 * styles, the matched-rule and declaration counts, and the matched-properties cache counts,
 * whose keys are the matched rules.
 */
void ExpectSameFilterIndependentPass(const CascadePassSnapshot& filtered,
                                     const CascadePassSnapshot& unfiltered) {
  EXPECT_EQ(unfiltered.counters.ancestorFilterRejections, 0u);
  EXPECT_EQ(unfiltered.counters.ancestorFilterFalsePositives, 0u);
  EXPECT_EQ(unfiltered.counters.ancestorFilterSaturations, 0u);
  EXPECT_EQ(unfiltered.counters.ancestorFilterKeyLimits, 0u);
  EXPECT_LE(
      filtered.counters.ancestorFilterRejections + filtered.counters.ancestorFilterFalsePositives,
      filtered.counters.rulesTested - filtered.counters.rulesMatched);
  if (unfiltered.styleBudgetRejected) {
    ::testing::Test::RecordProperty("unfiltered_budget_rejected", "true");
    EXPECT_GE(filtered.counters.rulesMatched, unfiltered.counters.rulesMatched);
    return;
  }
  EXPECT_THAT(filtered.styles, ElementsAreArray(unfiltered.styles));
  EXPECT_THAT(filtered.rules, ElementsAreArray(unfiltered.rules));
  EXPECT_THAT(filtered.warnings, ElementsAreArray(unfiltered.warnings));
  EXPECT_EQ(WithoutAncestorFilterCounters(filtered.counters),
            WithoutAncestorFilterCounters(unfiltered.counters));
  EXPECT_EQ(filtered.warningAttempts, unfiltered.warningAttempts);
  EXPECT_EQ(filtered.warningBytes, unfiltered.warningBytes);
  EXPECT_EQ(filtered.warningLimitExceeded, unfiltered.warningLimitExceeded);
  EXPECT_EQ(filtered.styleBudgetRejected, unfiltered.styleBudgetRejected);
}

/// The innermost last element of \p document: its last child element's last child element, and
/// so on, an element of the authored tree that an edit can address.
SVGElement LastLeafElement(SVGDocument& document) {
  SVGElement element = document.svgElement();
  while (std::optional<SVGElement> child = element.lastChild()) {
    element = *child;
  }
  return element;
}

/// A frame prepared after an edit, and how the render tree was updated for it.
struct EditedFrame {
  CascadePassSnapshot pass;
  std::uint64_t fullRebuilds = 0;
  std::uint64_t retainedUpdates = 0;
};

/**
 * Write a new `opacity` on \p element and prepare the next frame of \p document.
 *
 * @param retained Whether the frame may take the retained update; without it the render tree is
 *   rebuilt, restyling only the dirty element unless the edit needs a full restyle.
 */
EditedFrame EditOpacityAndPrepareFrame(SVGDocument& document, SVGElement element, bool retained) {
  Registry& registry = document.registry();
  registry.ctx().get<RenderTreeState>().retainedUpdatesEnabled = retained;
  const RenderTreeState::Counters before = registry.ctx().get<RenderTreeState>().counters;
  element.setAttribute("opacity", retained ? "0.5" : "0.25");
  CascadePassSnapshot pass = PrepareFirstFrameWithDiagnostics(document);
  const RenderTreeState::Counters after = registry.ctx().get<RenderTreeState>().counters;
  return EditedFrame{std::move(pass), after.fullRebuilds - before.fullRebuilds,
                     after.retainedUpdates - before.retainedUpdates};
}

TEST(MatchedPropertiesCache, RetainedUpdateAndPartialRebuildMatchCacheDisabled) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
      <style>rect { fill: blue }</style>
      <rect id="edited" width="8" height="8"/>
      <circle cx="12" cy="12" r="4"/>
    </svg>)svg";
  const auto runEdit = [&](bool disableCache, bool retainedUpdatesEnabled) {
    SVGDocument document = ParseSource(kSource);
    Registry& registry = document.registry();
    registry.ctx().emplace<RenderTreeState>().retainedUpdatesEnabled = retainedUpdatesEnabled;
    if (disableCache) {
      registry.ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
    }
    std::ignore = PrepareFirstFrameWithDiagnostics(document);
    const RenderTreeState::Counters before = registry.ctx().get<RenderTreeState>().counters;
    document.querySelector("#edited")->setAttribute("opacity", "0.5");
    CascadePassSnapshot pass = PrepareFirstFrameWithDiagnostics(document);
    const RenderTreeState::Counters after = registry.ctx().get<RenderTreeState>().counters;
    return std::tuple(std::move(pass), after.fullRebuilds - before.fullRebuilds,
                      after.retainedUpdates - before.retainedUpdates);
  };

  auto [cachedRetained, cachedRetainedFulls, cachedRetainedUpdates] = runEdit(false, true);
  auto [disabledRetained, disabledRetainedFulls, disabledRetainedUpdates] = runEdit(true, true);
  ExpectSameCacheIndependentPass(cachedRetained, disabledRetained);
  EXPECT_EQ(cachedRetainedFulls, 0u);
  EXPECT_EQ(disabledRetainedFulls, 0u);
  EXPECT_EQ(cachedRetainedUpdates, 1u);
  EXPECT_EQ(disabledRetainedUpdates, 1u);

  auto [cachedPartial, cachedPartialFulls, cachedPartialRetained] = runEdit(false, false);
  auto [disabledPartial, disabledPartialFulls, disabledPartialRetained] = runEdit(true, false);
  ExpectSameCacheIndependentPass(cachedPartial, disabledPartial);
  EXPECT_EQ(cachedPartialFulls, 1u);
  EXPECT_EQ(disabledPartialFulls, 1u);
  EXPECT_EQ(cachedPartialRetained, 0u);
  EXPECT_EQ(disabledPartialRetained, 0u);
  EXPECT_EQ(cachedPartial.counters.elementsCascaded, 1u);
  EXPECT_EQ(disabledPartial.counters.elementsCascaded, 1u);
}

/// Edit `opacity` on the last leaf element of \p filtered and \p unfiltered and expect the same
/// frame, prepared through the retained update and then through a render-tree rebuild that
/// restyles only the edited element.
void ExpectSameEditedFramesWithoutTheFilter(SVGDocument& filtered, SVGDocument& unfiltered) {
  const SVGElement edited = LastLeafElement(filtered);
  const SVGElement editedUnfiltered = LastLeafElement(unfiltered);
  for (const bool retained : {true, false}) {
    SCOPED_TRACE(retained ? "retained update" : "partial rebuild");
    const EditedFrame filteredFrame = EditOpacityAndPrepareFrame(filtered, edited, retained);
    const EditedFrame unfilteredFrame =
        EditOpacityAndPrepareFrame(unfiltered, editedUnfiltered, retained);
    ExpectSameFilterIndependentPass(filteredFrame.pass, unfilteredFrame.pass);
    EXPECT_EQ(filteredFrame.fullRebuilds, unfilteredFrame.fullRebuilds);
    EXPECT_EQ(filteredFrame.retainedUpdates, unfilteredFrame.retainedUpdates);
  }
}

/// Prepare a frame of \p indexed and of \p unindexed, two parses of one source, the second
/// testing every rule, and expect the same cascade.
void ExpectIndexedMatchesEveryRule(SVGDocument& indexed, SVGDocument& unindexed,
                                   SVGDocument* cacheDisabled = nullptr,
                                   SVGDocument* unfiltered = nullptr) {
  // The oracle skips no rule, so it runs without the ancestor filter as well.
  unindexed.registry().ctx().emplace<UnindexedCascadeForTesting>();
  unindexed.registry().ctx().emplace<UnfilteredCascadeForTesting>();
  if (cacheDisabled) {
    cacheDisabled->registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
  }
  if (unfiltered) {
    unfiltered->registry().ctx().emplace<UnfilteredCascadeForTesting>();
  }
  bool unindexedBudgetRejected = false;
  std::uint64_t indexedRulesMatched = 0;
  std::uint64_t unindexedRulesMatched = 0;
  {
    const CascadePassSnapshot indexedFirst = PrepareFirstFrameWithDiagnostics(indexed);
    const CascadePassSnapshot unindexedFirst = PrepareFirstFrameWithDiagnostics(unindexed);
    if (cacheDisabled) {
      const CascadePassSnapshot disabledFirst = PrepareFirstFrameWithDiagnostics(*cacheDisabled);
      ExpectSameCacheIndependentPass(indexedFirst, disabledFirst);
      EXPECT_EQ(disabledFirst.counters.matchedPropertiesCacheHits, 0u);
      EXPECT_EQ(disabledFirst.counters.matchedPropertiesCacheMisses, 0u);
      EXPECT_EQ(disabledFirst.counters.matchedPropertiesCacheUncacheable, 0u);
      EXPECT_EQ(disabledFirst.counters.declarationParsesSkipped, 0u);
    }
    if (unfiltered) {
      ExpectSameFilterIndependentPass(indexedFirst, PrepareFirstFrameWithDiagnostics(*unfiltered));
    }
    unindexedBudgetRejected = unindexedFirst.counters.budgetRejections > 0;
    indexedRulesMatched = indexedFirst.counters.rulesMatched;
    unindexedRulesMatched = unindexedFirst.counters.rulesMatched;
    if (!unindexedBudgetRejected) {
      EXPECT_THAT(indexedFirst.styles, ElementsAreArray(unindexedFirst.styles));
      EXPECT_THAT(indexedFirst.rules, ElementsAreArray(unindexedFirst.rules));
      EXPECT_EQ(indexedFirst.counters.rulesMatched, unindexedFirst.counters.rulesMatched);
      EXPECT_EQ(indexedFirst.counters.declarationsApplied,
                unindexedFirst.counters.declarationsApplied);
    }
  }
  const auto compareStage = [&](auto stage) {
    const CascadePassSnapshot reference = stage(indexed);
    if (cacheDisabled) {
      ExpectSameCacheIndependentPass(reference, stage(*cacheDisabled));
    }
    if (unfiltered) {
      ExpectSameFilterIndependentPass(reference, stage(*unfiltered));
    }
  };
  compareStage(ForceFullRestyleWithDiagnostics);
  compareStage(RecomputeLastPositionedStyleWithDiagnostics);
  if (unfiltered) {
    ExpectSameEditedFramesWithoutTheFilter(indexed, *unfiltered);
  }

  if (unindexedBudgetRejected) {
    // Testing every rule exhausted the style budget; the index tests fewer rules, so it may style
    // further but never less.
    ::testing::Test::RecordProperty("unindexed_budget_rejected", "true");
    EXPECT_GE(indexedRulesMatched, unindexedRulesMatched);
    return;
  }
}

/// The icon sprite, which is compared in parts (see \ref SpritePart).
constexpr std::string_view kSpriteName = "bootstrap_icons";

/// The corpus without the icon sprite.
std::vector<CorpusDocument> CorpusDocumentsExceptTheSprite() {
  std::vector<CorpusDocument> documents = tests::CorpusDocuments();
  std::erase_if(documents,
                [](const CorpusDocument& document) { return document.name == kSpriteName; });
  return documents;
}

/// Two draws on independent documents. The reference rebuilds its styles and render tree even
/// when the tested mutation would normally take the retained path.
void CompareAfterCorpusEdit(SVGDocument& actual, SVGDocument& reference, Renderer& renderer,
                            Renderer& referenceRenderer, std::string_view artifactName) {
  auto& state = reference.registry().ctx().get<RenderTreeState>();
  state.retainedUpdatesEnabled = false;
  state.needsFullStyleRecompute = true;
  state.needsFullRebuild = true;
  renderer.draw(actual);
  referenceRenderer.draw(reference);
  const auto actualStyles = PositionedStyles(actual);
  const auto expectedStyles = PositionedStyles(reference);
  ASSERT_THAT(actualStyles, ::testing::SizeIs(expectedStyles.size()));
  // The full sprite has thousands of records. Construct a diagnostic matcher only for a mismatch.
  const auto mismatch =
      std::mismatch(actualStyles.begin(), actualStyles.end(), expectedStyles.begin());
  if (mismatch.first != actualStyles.end()) {
    EXPECT_THAT(*mismatch.first, ::testing::Eq(*mismatch.second)) << mismatch.second->position;
  }
  EXPECT_THAT(PositionedRenderInstances(actual),
              ElementsAreArray(PositionedRenderInstances(reference)));
  const RendererBitmap expected = referenceRenderer.takeSnapshot();
  const RendererBitmap observed = renderer.takeSnapshot();
  if (expected.empty()) {
    // Documents rejected by the render size limit still have comparable styles and instances.
    EXPECT_THAT(observed.dimensions, ::testing::Eq(expected.dimensions));
    EXPECT_THAT(observed.pixels, ElementsAreArray(expected.pixels));
  } else {
    editor::tests::CompareBitmapToBitmap(observed, expected, artifactName,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// Authored elements in breadth-first order, excluding shadow clones. Two parses of one document
/// produce the same targets, even when their render trees use different entity identifiers.
std::vector<SVGElement> AuthoredElements(SVGDocument& document) {
  std::vector<SVGElement> result{document.svgElement()};
  for (std::size_t i = 0; i < result.size(); ++i) {
    for (auto child = result[i].firstChild(); child; child = child->nextSibling()) {
      result.push_back(*child);
    }
  }
  return result;
}

struct CorpusEditTargets {
  std::size_t leaf = 0;
  std::optional<std::size_t> withClass;
  std::map<std::string, std::size_t> presentation;
};

/// Start at a stable offset for each document (32-bit FNV-1a). Sample one authored leaf, one
/// element with an existing class when possible, and one occurrence per presentation attribute.
/// The targeted fixtures separately exercise particular selector directions and fallback paths.
CorpusEditTargets SelectCorpusEditTargets(const std::vector<SVGElement>& elements,
                                          std::string_view name) {
  std::uint32_t hash = 2166136261u;
  for (const unsigned char c : name) {
    hash = (hash ^ c) * 16777619u;
  }
  const std::size_t start = hash % elements.size();
  CorpusEditTargets targets;
  std::optional<std::size_t> firstLeaf;
  for (std::size_t offset = 0; offset < elements.size(); ++offset) {
    const std::size_t index = (start + offset) % elements.size();
    const auto& element = elements[index];
    if (!firstLeaf && !element.firstChild()) {
      firstLeaf = index;
    }
    if (!targets.withClass && !element.className().empty()) {
      targets.withClass = index;
    }
    for (const auto& attribute : element.attributes()) {
      if (attribute.namespacePrefix.empty() &&
          PropertyRegistry::isPresentationAttributeName(attribute.name)) {
        targets.presentation.try_emplace(std::string(std::string_view(attribute.name)), index);
      }
    }
  }
  targets.leaf = firstLeaf.value_or(start);
  return targets;
}

class StyleEditCorpusTest : public ::testing::TestWithParam<CorpusDocument> {
protected:
  void SetUp() override {
    actual_ = tests::ParseCorpusDocument(GetParam());
    reference_ = tests::ParseCorpusDocument(GetParam());
    if (!actual_ || !reference_) {
      GTEST_SKIP() << "Does not parse: " << GetParam();
    }
    for (SVGDocument* document : {&*actual_, &*reference_}) {
      document->setCanvasSize(48, 48);
    }
    renderer_.draw(*actual_);
    referenceRenderer_.draw(*reference_);
    elements_ = AuthoredElements(*actual_);
    mirrors_ = AuthoredElements(*reference_);
    ASSERT_THAT(elements_, ::testing::Not(::testing::IsEmpty()));
    ASSERT_THAT(mirrors_, ::testing::SizeIs(elements_.size()));
    targets_ = SelectCorpusEditTargets(elements_, GetParam().name);
  }

  template <typename Edit>
  void compare(std::string_view operation, std::size_t index, Edit edit) {
    SCOPED_TRACE(::testing::Message()
                 << GetParam().name << ", " << operation << ", target " << index);
    edit(elements_[index]);
    edit(mirrors_[index]);
    std::string artifactName = GetParam().name + "_" + std::string(operation) + "_" +
                               std::to_string(index) + "_" + std::to_string(editOrdinal_++);
    for (char& ch : artifactName) {
      constexpr std::string_view kSafe =
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
      if (kSafe.find(ch) == std::string_view::npos) {
        ch = '_';
      }
    }
    CompareAfterCorpusEdit(*actual_, *reference_, renderer_, referenceRenderer_, artifactName);
  }

  CorpusEditTargets targets_;

private:
  std::optional<SVGDocument> actual_, reference_;
  std::vector<SVGElement> elements_, mirrors_;
  Renderer renderer_, referenceRenderer_;
  std::size_t editOrdinal_ = 0;
};

TEST_P(StyleEditCorpusTest, ClassAndIdEditsMatchFullRebuild) {
  const std::size_t withClass = targets_.withClass.value_or(targets_.leaf);
  compare("add class", targets_.leaf, [](auto& e) {
    e.setClassName(std::string(std::string_view(e.className())) + " corpus-edited");
  });
  compare("replace class", withClass, [](auto& e) { e.setClassName("corpus-replaced"); });
  compare("remove class", withClass, [](auto& e) { e.removeAttribute("class"); });
  compare("set id", targets_.leaf, [](auto& e) { e.setId("corpus-edited-id"); });
}

TEST_P(StyleEditCorpusTest, InlineAndFillEditsMatchFullRebuild) {
  compare("inline style", targets_.leaf,
          [](auto& e) { e.setStyle("fill:#196ab2;stroke:#9d412b"); });
  compare("update inline style", targets_.leaf, [](auto& e) { e.updateStyle("fill:#37c29a"); });
  compare("set fill", targets_.leaf, [](auto& e) { e.setAttribute("fill", "#357fb9"); });
}

TEST_P(StyleEditCorpusTest, RemovingAuthoredPresentationMatchesFullRebuild) {
  for (const auto& [attribute, index] : targets_.presentation) {
    compare(attribute, index,
            [&](auto& e) { e.removeAttribute(xml::XMLQualifiedNameRef(attribute)); });
  }
}

INSTANTIATE_TEST_SUITE_P(Corpus, StyleEditCorpusTest, ::testing::ValuesIn(tests::CorpusDocuments()),
                         [](const ::testing::TestParamInfo<CorpusDocument>& info) {
                           return info.param.name;
                         });

class StyleIndexDifferentialTest : public ::testing::TestWithParam<CorpusDocument> {};

TEST_P(StyleIndexDifferentialTest, IndexedCascadeMatchesEveryRuleCascade) {
  ASSERT_THAT(std::filesystem::file_size(GetParam().path), ::testing::Gt(0u)) << GetParam();
  std::optional<SVGDocument> indexed = tests::ParseCorpusDocument(GetParam());
  std::optional<SVGDocument> unindexed = tests::ParseCorpusDocument(GetParam());
  std::optional<SVGDocument> cacheDisabled = tests::ParseCorpusDocument(GetParam());
  std::optional<SVGDocument> unfiltered = tests::ParseCorpusDocument(GetParam());
  if (!indexed || !unindexed || !cacheDisabled || !unfiltered) {
    GTEST_SKIP() << "Does not parse";
  }
  ExpectIndexedMatchesEveryRule(*indexed, *unindexed, &*cacheDisabled, &*unfiltered);
}

INSTANTIATE_TEST_SUITE_P(Corpus, StyleIndexDifferentialTest,
                         ::testing::ValuesIn(CorpusDocumentsExceptTheSprite()),
                         [](const ::testing::TestParamInfo<CorpusDocument>& info) {
                           return info.param.name;
                         });

/// Selector traversal steps allowed per pass in the low-limit run: low enough that the larger
/// documents end their unfiltered first frame traversal-rejected.
constexpr std::size_t kLowTraversalSteps = 64;

class StyleFilterLowTraversalTest : public ::testing::TestWithParam<CorpusDocument> {};

TEST_P(StyleFilterLowTraversalTest, FilterStylesAtLeastAsFarUnderALowTraversalLimit) {
  std::optional<SVGDocument> filtered = tests::ParseCorpusDocument(GetParam());
  std::optional<SVGDocument> unfiltered = tests::ParseCorpusDocument(GetParam());
  if (!filtered || !unfiltered) {
    GTEST_SKIP() << "Does not parse";
  }
  StyleResourceBudget::Limits limits;
  limits.selectorTraversalSteps = kLowTraversalSteps;
  filtered->registry().ctx().emplace<StyleResourceBudget>(limits);
  unfiltered->registry().ctx().emplace<StyleResourceBudget>(limits);
  unfiltered->registry().ctx().emplace<UnfilteredCascadeForTesting>();
  const CascadePassSnapshot filteredFirst = PrepareFirstFrameWithDiagnostics(*filtered);
  const CascadePassSnapshot unfilteredFirst = PrepareFirstFrameWithDiagnostics(*unfiltered);
  ::testing::Test::RecordProperty("filtered_budget_rejected",
                                  filteredFirst.styleBudgetRejected ? "true" : "false");
  ExpectSameFilterIndependentPass(filteredFirst, unfilteredFirst);
}

INSTANTIATE_TEST_SUITE_P(Corpus, StyleFilterLowTraversalTest,
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

/// Parts the icon sprite is compared in (see \ref SpritePart).
constexpr int kSpriteParts = 4;

/// Offset of the first `<symbol>` of part \p part of \p source's symbols under its root, or the
/// root's closing tag for \p part == \ref kSpriteParts.
std::size_t SpritePartStart(const std::string& source, std::size_t rootOpenEnd,
                            std::size_t rootClose, int part) {
  if (part == kSpriteParts) {
    return rootClose;
  }
  return source.find("<symbol", rootOpenEnd + (rootClose - rootOpenEnd) * part / kSpriteParts);
}

/**
 * One of \ref kSpriteParts consecutive parts of the icon sprite's `<symbol>` elements under its
 * root `<svg>`. The sprite has no stylesheet of its own and no selector of the user agent sheet
 * relates one symbol to another, so each part cascades its elements exactly as the whole sprite
 * does; together the parts hold every element. Whole, the sprite's four parses and their passes
 * are the slowest case of this test under ASan, about 70 s; quarters keep each case near a
 * quarter of that.
 *
 * @param part 0 to \ref kSpriteParts - 1.
 */
std::string SpritePart(int part) {
  const std::string source = SpriteSource();
  const std::size_t rootOpenEnd = source.find('>', source.find("<svg")) + 1;
  const std::size_t rootClose = source.rfind("</svg>");
  const std::size_t start = SpritePartStart(source, rootOpenEnd, rootClose, part);
  const std::size_t end = SpritePartStart(source, rootOpenEnd, rootClose, part + 1);
  EXPECT_LT(start, end);
  EXPECT_LE(end, rootClose);
  // The first part also keeps whatever precedes the first symbol.
  const std::size_t from = part == 0 ? rootOpenEnd : start;
  return source.substr(0, rootOpenEnd) + source.substr(from, end - from) + "</svg>";
}

class StyleIndexSpriteTest : public ::testing::TestWithParam<int> {};

TEST_P(StyleIndexSpriteTest, IndexedCascadeMatchesEveryRuleCascade) {
  const std::string source = SpritePart(GetParam());
  ASSERT_THAT(source, ::testing::HasSubstr("<symbol"));
  SVGDocument indexed = ParseSource(source);
  SVGDocument unindexed = ParseSource(source);
  SVGDocument cacheDisabled = ParseSource(source);
  SVGDocument unfiltered = ParseSource(source);
  ExpectIndexedMatchesEveryRule(indexed, unindexed, &cacheDisabled, &unfiltered);
}

INSTANTIATE_TEST_SUITE_P(Parts, StyleIndexSpriteTest, ::testing::Range(0, kSpriteParts));

TEST(StyleIndexSprite, PartsTogetherAreTheWholeSprite) {
  const std::string source = SpriteSource();
  constexpr std::string_view kRootClose = "</svg>";
  const std::size_t rootOpenEnd = source.find('>', source.find("<svg")) + 1;
  // The parts without their root's opening and closing tags, in order, after the sprite's
  // opening tag and before its closing tag, are the sprite up to its closing tag.
  std::string joined = source.substr(0, rootOpenEnd);
  for (int part = 0; part < kSpriteParts; ++part) {
    const std::string piece = SpritePart(part);
    ASSERT_GT(piece.size(), rootOpenEnd + kRootClose.size()) << part;
    joined += piece.substr(rootOpenEnd, piece.size() - rootOpenEnd - kRootClose.size());
  }
  EXPECT_EQ(joined + std::string(kRootClose),
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
  SVGDocument cacheDisabled = parse();
  SVGDocument unfiltered = parse();
  unindexed.registry().ctx().emplace<UnindexedCascadeForTesting>();
  unindexed.registry().ctx().emplace<UnfilteredCascadeForTesting>();
  cacheDisabled.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
  unfiltered.registry().ctx().emplace<UnfilteredCascadeForTesting>();
  for (SVGDocument* document : {&indexed, &unindexed, &cacheDisabled, &unfiltered}) {
    for (const auto& [id, className] : GetParam().classWrites) {
      std::optional<SVGElement> element = document->querySelector("#" + id);
      ASSERT_TRUE(element.has_value()) << id;
      element->setClassName(className);
    }
    PrepareFrame(*document);
  }
  EXPECT_EQ(Counters(unindexed).rulesMatched, GetParam().rulesMatched);
  ExpectSameCascade(indexed, unindexed);
  ExpectSameCascade(indexed, cacheDisabled);
  ExpectSameCascade(indexed, unfiltered);
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
  SVGDocument cacheDisabled = ParseSource(kSource);
  unindexed.registry().ctx().emplace<UnindexedCascadeForTesting>();
  unindexed.registry().ctx().emplace<UnfilteredCascadeForTesting>();
  cacheDisabled.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
  for (SVGDocument* document : {&indexed, &unindexed, &cacheDisabled}) {
    PrepareFrame(*document);
  }
  ExpectSameCascade(indexed, unindexed);
  ExpectSameCascade(indexed, cacheDisabled);
  const auto fillOf = [](SVGDocument& document) {
    return document.querySelector("#r")->getComputedStyle().fill.get();
  };
  const auto solid = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return PaintServer(PaintServer::Solid(css::Color(css::RGBA(r, g, b, 0xFF))));
  };
  EXPECT_THAT(fillOf(indexed), ::testing::Optional(solid(0, 0, 0)));

  // `.a` becomes `.b`, which the rect carries.
  for (SVGDocument* document : {&indexed, &unindexed, &cacheDisabled}) {
    const auto rect = document->querySelector("#r")->unsafeEntityHandle();
    const std::weak_ptr<const ComputedStyle> oldRecord = rect.get<ComputedStyleComponent>().style;
    ASSERT_THAT(oldRecord.expired(), ::testing::Eq(false));
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
    // A later full render also clears styles. Check the stylesheet invalidation itself.
    EXPECT_THAT(oldRecord.expired(), ::testing::Eq(true));
    PrepareFrame(*document);
  }
  ExpectSameCascade(indexed, unindexed);
  ExpectSameCascade(indexed, cacheDisabled);
  EXPECT_THAT(fillOf(indexed), ::testing::Optional(solid(0xFF, 0, 0)));

  for (SVGDocument* document : {&indexed, &unindexed, &cacheDisabled}) {
    document->querySelector("style")->cast<SVGStyleElement>().setContents(
        "#r { fill: blue } circle { stroke: green } [class] { opacity: 0.5 }");
    PrepareFrame(*document);
  }
  ExpectSameCascade(indexed, unindexed);
  ExpectSameCascade(indexed, cacheDisabled);
  EXPECT_THAT(fillOf(indexed), ::testing::Optional(solid(0, 0, 0xFF)));

  // Same number of rules, different keys: the index must be rebuilt, not kept by its size.
  for (SVGDocument* document : {&indexed, &unindexed, &cacheDisabled}) {
    document->querySelector("style")->cast<SVGStyleElement>().setContents(
        "#q { fill: blue } rect { fill: green } [id] { opacity: 0.5 }");
    PrepareFrame(*document);
  }
  ExpectSameCascade(indexed, unindexed);
  ExpectSameCascade(indexed, cacheDisabled);
  EXPECT_THAT(fillOf(indexed), ::testing::Optional(solid(0, 0x80, 0)));

  // Keep the matching rule positions and specificity while changing a declaration.
  for (SVGDocument* document : {&indexed, &unindexed, &cacheDisabled}) {
    document->querySelector("style")->cast<SVGStyleElement>().setTextContent(
        "#q { fill: blue } rect { fill: red } [id] { opacity: 0.5 }");
    auto rect = document->querySelector("#r");
    ASSERT_THAT(rect, testing::Optional(testing::_));
    rect->setStyle("");
    EXPECT_THAT(fillOf(*document), testing::Optional(solid(0xFF, 0, 0)));
    PrepareFrame(*document);
  }
  ExpectSameCascade(indexed, unindexed);
  ExpectSameCascade(indexed, cacheDisabled);
}

TEST(StyleIndexDifferential, SourceRuleEditMatchesOldAndNewSubtreesAndUseInstances) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <style>.old { fill: red; } .control { fill: blue; }</style>
      <g class="old"><rect id="loses" width="5" height="5"/></g>
      <defs><g id="template" class="new"><rect width="5" height="5"/></g></defs>
      <use href="#template" x="10"/>
      <rect id="untouched" class="control" x="20" width="5" height="5"/>
    </svg>)svg";
  auto actual = ParseSource(kSource);
  Renderer renderer;
  renderer.draw(actual);
  const auto control = actual.querySelector("#untouched");
  ASSERT_THAT(control, testing::Optional(testing::_));
  const EntityHandle controlHandle = control->unsafeEntityHandle();
  const auto untouched = controlHandle.get<ComputedStyleComponent>().style;
  ASSERT_THAT(untouched.get(), testing::NotNull());
  const auto before = Counters(actual);
  const std::size_t start = kSource.find(".old");
  ASSERT_THAT(start, testing::Ne(std::string_view::npos));
  const auto edit = actual.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(start + 4)},
      .replacement = ".new",
      .sourceVersion = actual.sourceVersion(),
  });
  ASSERT_THAT(edit.applied, testing::IsTrue());

  auto expected = ParseSource(actual.source());
  Renderer reference;
  reference.draw(expected);
  renderer.draw(actual);
  const auto work = Counters(actual) - before;
  EXPECT_THAT(work.fullClears, testing::Eq(0u));
  EXPECT_THAT(work.elementsCascaded, testing::Gt(0u));
  EXPECT_THAT(controlHandle.get<ComputedStyleComponent>().style.get(),
              testing::Eq(untouched.get()));
  ExpectSameStyleOutputs(actual, expected);
  EXPECT_THAT(PositionedRenderInstances(actual),
              ElementsAreArray(PositionedRenderInstances(expected)));
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference.takeSnapshot(),
                                       "stylesheet_rule_source_edit",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(StyleIndexDifferential, SourceCustomPropertyEditSelectivelyRestylesItsConsumers) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>.hot { --tone: #f00; } rect { fill: var(--tone, green); }</style>
      <g class="hot"><rect id="edited" width="8" height="8"/></g>
      <rect id="clean" x="8" width="8" height="8"/>
    </svg>)svg";
  auto actual = ParseSource(source);
  Renderer renderer;
  renderer.draw(actual);
  const auto control = actual.querySelector("#clean");
  ASSERT_THAT(control, testing::Optional(testing::_));
  const auto cleanHandle = control->unsafeEntityHandle();
  const auto cleanRecord = cleanHandle.get<ComputedStyleComponent>().style;
  const auto before = Counters(actual);
  const auto start = source.find("#f00");
  ASSERT_THAT(start, testing::Ne(std::string_view::npos));
  const auto edit = actual.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(start + 4)},
      .replacement = "#00f",
      .sourceVersion = actual.sourceVersion(),
  });
  ASSERT_THAT(edit.applied, testing::IsTrue());
  auto expected = ParseSource(actual.source());
  Renderer reference;
  reference.draw(expected);
  renderer.draw(actual);
  const auto work = Counters(actual) - before;
  EXPECT_THAT(work.fullClears, testing::Eq(0u));
  EXPECT_THAT(work.elementsCascaded, testing::Eq(2u));
  EXPECT_THAT(cleanHandle.get<ComputedStyleComponent>().style.get(),
              testing::Eq(cleanRecord.get()));
  ExpectSameStyleOutputs(actual, expected);
  const auto target = actual.querySelector("#edited");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const PaintServer blue = PaintServer::Solid(css::Color(css::RGBA(0, 0, 255, 255)));
  EXPECT_THAT(target->getComputedStyle().fill.get(), testing::Optional(blue));
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference.takeSnapshot(),
                                       "stylesheet_selective_custom_value",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(StyleIndexDifferential, SourceRuleEditPreservesShiftedCustomTokenOffsets) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>.old { fill: red; } .spacer { fill: blue; } #r { --tone: rgb(0, 0, 255); fill: var(--tone); }</style>
      <rect id="r" width="8" height="8"/>
    </svg>)svg";
  auto actual = ParseSource(kSource);
  Renderer renderer;
  renderer.draw(actual);
  const auto before = Counters(actual);
  const std::size_t start = kSource.find(".old");
  ASSERT_THAT(start, testing::Ne(std::string_view::npos));
  const auto edit = actual.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start + 4), FileOffset::Offset(start + 4)},
      .replacement = "er",
      .sourceVersion = actual.sourceVersion(),
  });
  ASSERT_THAT(edit.applied, testing::IsTrue());
  auto expected = ParseSource(actual.source());
  Renderer reference;
  reference.draw(expected);
  renderer.draw(actual);
  EXPECT_THAT((Counters(actual) - before).fullClears, testing::Eq(1u));
  ExpectSameStyleOutputs(actual, expected);

  const auto target = actual.querySelector("#r");
  const auto referenceTarget = expected.querySelector("#r");
  ASSERT_THAT(target, testing::Optional(testing::_));
  ASSERT_THAT(referenceTarget, testing::Optional(testing::_));
  const auto tokenOffset = [](const SVGElement& element) -> std::optional<FileOffset> {
    const auto* slot = element.unsafeEntityHandle().try_get<ComputedStyleComponent>();
    if (!slot || !slot->style || !slot->style->customProperties) {
      return std::nullopt;
    }
    const auto* sequence = slot->style->customProperties->find("--tone");
    return sequence && !sequence->tokens().empty()
               ? std::optional(sequence->tokens().front().sourceOffset())
               : std::nullopt;
  };
  const auto freshOffset = tokenOffset(*referenceTarget);
  ASSERT_THAT(freshOffset, testing::Optional(testing::_));
  EXPECT_THAT(tokenOffset(*target), testing::Eq(freshOffset));
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference.takeSnapshot(),
                                       "stylesheet_shifted_custom_values",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(StyleIndexDifferential, SourceRuleEditMarksPendingOpacityTargetForGeometryRebuild) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>#r { width: 4px; height: 8px; fill: red; }</style>
      <rect id="r" />
    </svg>)svg";
  auto actual = ParseSource(source);
  Renderer renderer;
  renderer.draw(actual);
  auto target = actual.querySelector("#r");
  ASSERT_THAT(target, testing::Optional(testing::_));
  target->setAttribute("opacity", "0.5");
  const auto handle = target->unsafeEntityHandle();
  ASSERT_THAT(handle.try_get<ComputedStyleComponent>(), testing::IsNull());
  const auto paintDirty = [&] {
    return handle.get<DirtyFlagsComponent>().test(DirtyFlagsComponent::Paint);
  };
  ASSERT_THAT(paintDirty(), testing::IsFalse());

  const auto start = std::string_view(actual.source()).find("4px");
  ASSERT_THAT(start, testing::Ne(std::string_view::npos));
  const auto edit = actual.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(start + 1)},
      .replacement = "8",
      .sourceVersion = actual.sourceVersion(),
  });
  ASSERT_THAT(edit.applied, testing::IsTrue());
  EXPECT_THAT(paintDirty(), testing::IsTrue());

  auto expected = ParseSource(actual.source());
  auto referenceTarget = expected.querySelector("#r");
  ASSERT_THAT(referenceTarget, testing::Optional(testing::_));
  referenceTarget->setAttribute("opacity", "0.5");
  Renderer reference;
  reference.draw(expected);
  renderer.draw(actual);
  ExpectSameStyleOutputs(actual, expected);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference.takeSnapshot(),
                                       "stylesheet_geometry_after_pending_opacity",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(StyleIndexDifferential, SourceRuleEditLayerOrderUsesCanonicalFrame) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>
        @layer base, theme;
        @layer base { #r { --tone: red; } }
        @layer theme { rect { --tone: blue; fill: var(--tone); } }
      </style>
      <rect id="r" width="8" height="8"/>
    </svg>)svg";
  auto actual = ParseSource(kSource);
  Renderer renderer;
  renderer.draw(actual);
  const auto before = Counters(actual);
  constexpr std::string_view order = "base, theme";
  const auto start = kSource.find(order);
  ASSERT_THAT(start, testing::Ne(std::string_view::npos));
  const auto edit = actual.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(start + order.size())},
      .replacement = "theme, base",
      .sourceVersion = actual.sourceVersion(),
  });
  ASSERT_THAT(edit.applied, testing::IsTrue());
  auto expected = ParseSource(actual.source());
  Renderer reference;
  reference.draw(expected);
  renderer.draw(actual);
  EXPECT_THAT((Counters(actual) - before).fullClears, testing::Eq(1u));
  ExpectSameStyleOutputs(actual, expected);
  const auto target = actual.querySelector("#r");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const PaintServer red = PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255)));
  EXPECT_THAT(target->getComputedStyle().fill.get(), testing::Optional(red));
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference.takeSnapshot(),
                                       "stylesheet_layer_order_edit",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(StyleIndexDifferential, SourceRuleHasEditFallsBackToCanonicalFrame) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>g:has(.old) { fill: red; }</style>
      <g><rect class="new" width="8" height="8"/></g>
    </svg>)svg";
  auto actual = ParseSource(kSource);
  Renderer renderer;
  renderer.draw(actual);
  const auto before = Counters(actual);
  const std::size_t start = kSource.find(".old");
  ASSERT_THAT(start, testing::Ne(std::string_view::npos));
  const auto edit = actual.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(start + 4)},
      .replacement = ".new",
      .sourceVersion = actual.sourceVersion(),
  });
  ASSERT_THAT(edit.applied, testing::IsTrue());

  auto expected = ParseSource(actual.source());
  Renderer reference;
  reference.draw(expected);
  renderer.draw(actual);
  EXPECT_THAT((Counters(actual) - before).fullClears, testing::Eq(1u));
  ExpectSameStyleOutputs(actual, expected);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference.takeSnapshot(),
                                       "stylesheet_has_source_edit",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(StyleIndexDifferential, SourceRuleEditPreservesShiftedDeferredValueOffsets) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>.old { fill: red; } .spacer { fill: blue; } #r { width: 8px; }</style>
      <rect id="r" class="new" height="8"/>
    </svg>)svg";
  auto actual = ParseSource(kSource);
  Renderer renderer;
  renderer.draw(actual);
  const auto before = Counters(actual);
  const std::size_t start = kSource.find(".old");
  ASSERT_THAT(start, testing::Ne(std::string_view::npos));
  const auto edit = actual.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start + 4), FileOffset::Offset(start + 4)},
      .replacement = "er",
      .sourceVersion = actual.sourceVersion(),
  });
  ASSERT_THAT(edit.applied, testing::IsTrue());

  auto expected = ParseSource(actual.source());
  Renderer reference;
  reference.draw(expected);
  renderer.draw(actual);
  EXPECT_THAT((Counters(actual) - before).fullClears, testing::Eq(1u));
  ExpectSameStyleOutputs(actual, expected);
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), reference.takeSnapshot(),
                                       "stylesheet_shifted_deferred_value",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(StyleIndexDifferential, SourceRuleEditAfterRefusalReplaysCanonicalFrame) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
      <style>.old { fill: red; } .control { fill: blue; }</style>
      <rect class="old" width="4" height="4"/>
      <rect class="control" x="8" width="4" height="4"/>
    </svg>)svg";
  auto actual = ParseSource(kSource);
  StyleResourceBudget::Limits initialLimits;
  initialLimits.ruleElementMatches = 1;
  auto& ctx = actual.registry().ctx();
  ctx.emplace<StyleResourceBudget>(initialLimits);
  const auto initial = PrepareFirstFrameWithDiagnostics(actual);
  ASSERT_THAT(initial.counters.budgetRejections, testing::Gt(0u));
  ctx.erase<StyleResourceBudget>();
  ctx.emplace<StyleResourceBudget>();

  const std::size_t start = kSource.find(".old");
  ASSERT_THAT(start, testing::Ne(std::string_view::npos));
  const auto edit = actual.applySourceEdit(xml::XMLEditIntent{
      .range = {FileOffset::Offset(start), FileOffset::Offset(start + 4)},
      .replacement = ".new",
      .sourceVersion = actual.sourceVersion(),
  });
  ASSERT_THAT(edit.applied, testing::IsTrue());

  auto expected = ParseSource(actual.source());
  const auto fresh = PrepareFirstFrameWithDiagnostics(expected);
  const auto replayed = PrepareFirstFrameWithDiagnostics(actual);
  EXPECT_THAT(replayed.counters.fullClears, testing::Eq(1u));
  EXPECT_THAT(replayed.styleBudgetRejected, testing::IsFalse());
  EXPECT_THAT(replayed.warnings, ElementsAreArray(fresh.warnings));
  ExpectSameStyleOutputs(actual, expected);
  EXPECT_THAT(PositionedRenderInstances(actual),
              ElementsAreArray(PositionedRenderInstances(expected)));
}

TEST(StyleIndexDifferential, StylesheetEditsReplaceAncestorHashes) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.bad #r { fill: red; }</style>
      <g class="hit"><rect id="r" width="1" height="1"/></g>
    </svg>)svg";
  SVGDocument filtered = ParseSource(kSource);
  SVGDocument unfiltered = ParseSource(kSource);
  unfiltered.registry().ctx().emplace<UnfilteredCascadeForTesting>();

  const auto check = [&](std::uint64_t rejections, const css::RGBA& rgba) {
    const auto filteredPass = PrepareFirstFrameWithDiagnostics(filtered);
    const auto unfilteredPass = PrepareFirstFrameWithDiagnostics(unfiltered);
    ExpectSameFilterIndependentPass(filteredPass, unfilteredPass);
    EXPECT_FALSE(unfilteredPass.styleBudgetRejected);
    EXPECT_EQ(filteredPass.counters.ancestorFilterRejections, rejections);
    EXPECT_THAT(filtered.querySelector("#r")->getComputedStyle().fill.get(),
                ::testing::Optional(PaintServer(PaintServer::Solid(css::Color(rgba)))));
  };
  check(1, css::RGBA(0, 0, 0, 0xFF));

  // Keep the same number of rules and the same subject key while making the ancestor match.
  for (SVGDocument* document : {&filtered, &unfiltered}) {
    const std::size_t start = std::string_view(document->source()).find(".bad");
    ASSERT_NE(start, std::string_view::npos);
    const auto edit = document->applySourceEdit(xml::XMLEditIntent{
        .range = {FileOffset::Offset(start + 1), FileOffset::Offset(start + 4)},
        .replacement = "hit",
        .sourceVersion = document->sourceVersion(),
    });
    ASSERT_TRUE(edit.applied);
  }
  check(0, css::RGBA(0xFF, 0, 0, 0xFF));

  for (SVGDocument* document : {&filtered, &unfiltered}) {
    document->querySelector("style")->cast<SVGStyleElement>().setContents(".bad #r { fill: red; }");
  }
  check(1, css::RGBA(0, 0, 0, 0xFF));
}

/// A document built around how the ancestor filter reads ancestor keys and selector hashes.
struct FilterEdgeCase {
  std::string name;
  /// Content of the `<svg>` root.
  std::string body;
  /// Candidates the filter rejects, and candidates it lets through that then fail, over all
  /// elements of the first frame.
  std::uint64_t rejections;
  std::uint64_t falsePositives;

  friend std::ostream& operator<<(std::ostream& os, const FilterEdgeCase& value) {
    return os << value.name;
  }
};

class StyleFilterEdgeCaseTest : public ::testing::TestWithParam<FilterEdgeCase> {};

TEST_P(StyleFilterEdgeCaseTest, FilteredCascadeMatchesUnfilteredCascade) {
  const std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">)" +
                             GetParam().body + "</svg>";
  SVGDocument filtered = ParseSource(source);
  SVGDocument unfiltered = ParseSource(source);
  unfiltered.registry().ctx().emplace<UnfilteredCascadeForTesting>();
  const CascadePassSnapshot filteredFirst = PrepareFirstFrameWithDiagnostics(filtered);
  const CascadePassSnapshot unfilteredFirst = PrepareFirstFrameWithDiagnostics(unfiltered);
  ExpectSameFilterIndependentPass(filteredFirst, unfilteredFirst);
  EXPECT_FALSE(unfilteredFirst.styleBudgetRejected);
  EXPECT_EQ(filteredFirst.counters.ancestorFilterRejections, GetParam().rejections);
  EXPECT_EQ(filteredFirst.counters.ancestorFilterFalsePositives, GetParam().falsePositives);
}

INSTANTIATE_TEST_SUITE_P(
    Ancestors, StyleFilterEdgeCaseTest,
    ::testing::Values(
        // A shadow element's chain runs through its `<use>` host: the clone under `g.host`
        // rejects only `.other rect`, while its source under `<defs>` rejects all three rules.
        FilterEdgeCase{"UseShadowAncestors",
                       "<style>.host rect { fill: red } .other rect { stroke: red }"
                       " g rect { opacity: 0.5 }</style>"
                       R"(<defs><rect id="src" width="1" height="1"/></defs>)"
                       R"(<g class="host"><use href="#src"/></g>)",
                       4, 0},
        FilterEdgeCase{"AttributeAncestor",
                       "<style>[data-k] rect { fill: red } [data-q] rect { stroke: red }</style>"
                       R"(<g data-k="1"><rect width="1" height="1"/></g>)",
                       1, 0},
        // Simple selectors inside pseudo-class arguments give no keys, so the first two rules
        // are never rejected; `.c:not(.d)` still gives `.c`.
        FilterEdgeCase{"PseudoClassArguments",
                       "<style>:not(.a) rect { fill: red } :is(.a, .b) rect { stroke: red }"
                       " .c:not(.d) rect { opacity: 0.5 }</style>"
                       R"(<g class="b"><rect width="1" height="1"/></g>)",
                       1, 0},
        // Keys stop at the first sibling combinator: `.b` is kept and `.a` or `.z` is not, so
        // `.z ~ .b rect` passes the filter and then fails.
        FilterEdgeCase{"SiblingCombinators",
                       "<style>.a + .b rect { fill: red } .z ~ .b rect { stroke: red }"
                       " .a + .c rect { opacity: 0.5 }</style>"
                       R"(<g class="a"/><g class="b"><rect width="1" height="1"/></g>)",
                       1, 1},
        FilterEdgeCase{"ChildAndId",
                       "<style>#top > rect { fill: red } #nope > rect { stroke: red }"
                       " .p > rect { opacity: 0.5 }</style>"
                       R"(<g id="top"><rect width="1" height="1"/></g>)",
                       2, 0},
        // Type keys are compared lowercased on both sides.
        FilterEdgeCase{"TypeAncestors",
                       "<style>g rect { fill: red } clipPath rect { opacity: 0.5 }"
                       " CLIPPATH circle { stroke: red }</style>"
                       R"(<defs><clipPath id="c"><rect width="1" height="1"/></clipPath></defs>)"
                       R"(<g><rect width="1" height="1"/><circle r="1"/></g>)",
                       3, 0},
        // A rule is rejected only when every selector-list entry is.
        FilterEdgeCase{"SelectorListEntries",
                       "<style>.nope rect, .x rect { fill: red }"
                       " .nope rect, .none rect { stroke: red }</style>"
                       R"(<g class="x"><rect width="1" height="1"/></g>)",
                       1, 0},
        // An entry is rejected when any one of its ancestor keys is absent.
        FilterEdgeCase{"EveryKeyOfAnEntry",
                       "<style>.x .nope rect { stroke: red } .x g rect { fill: red }</style>"
                       R"(<g class="x"><g><rect width="1" height="1"/></g></g>)",
                       1, 0}),
    [](const ::testing::TestParamInfo<FilterEdgeCase>& info) { return info.param.name; });

}  // namespace
}  // namespace donner::svg::components
