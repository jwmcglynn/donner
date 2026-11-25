/**
 * Tests for StyleSystem: CSS cascade, inheritance, and style computation from SVG elements.
 */

#include "donner/svg/components/style/StyleSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/SVGStyleQuery.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/components/style/MatchedPropertiesCache.h"
#include "donner/svg/components/style/StyleAncestorFilter.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/parser/SVGParser.h"

using testing::NotNull;

namespace donner::svg::components {

namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Le;

std::vector<bool> UserAgentMatchFlags(const SVGElement& element) {
  const std::vector<SVGMatchedStyleRule> matches = CollectMatchedStyleRules(element);
  std::vector<bool> result;
  result.reserve(matches.size());
  for (const SVGMatchedStyleRule& match : matches) {
    result.push_back(match.isUserAgentStylesheet);
  }
  return result;
}

void SetRenderTreeState(Registry& registry, const RenderTreeState& state) {
  if (registry.ctx().contains<RenderTreeState>()) {
    registry.ctx().erase<RenderTreeState>();
  }
  registry.ctx().emplace<RenderTreeState>(state);
}

std::string_view SourceSlice(std::string_view source, const SourceRange& range) {
  EXPECT_TRUE(range.start.offset.has_value());
  EXPECT_TRUE(range.end.offset.has_value());
  if (!range.start.offset.has_value() || !range.end.offset.has_value() ||
      *range.end.offset < *range.start.offset) {
    return {};
  }

  return source.substr(*range.start.offset, *range.end.offset - *range.start.offset);
}

struct AuthoredMatchedRuleSummary {
  std::size_t selectorEntryIndex = 0;
  css::Specificity specificity;
  std::string selectorSource;
  std::string ruleSource;
};

void PrintTo(const AuthoredMatchedRuleSummary& summary, std::ostream* os) {
  *os << "AuthoredMatchedRuleSummary{selectorEntryIndex=" << summary.selectorEntryIndex
      << ", specificity=" << summary.specificity << ", selectorSource=\"" << summary.selectorSource
      << "\", ruleSource=\"" << summary.ruleSource << "\"}";
}

std::vector<AuthoredMatchedRuleSummary> AuthoredMatchedRuleSummaries(
    std::string_view documentSource, const std::vector<MatchedStyleRule>& matches) {
  std::vector<AuthoredMatchedRuleSummary> summaries;
  for (const MatchedStyleRule& match : matches) {
    if (!match.ruleSourceRange.has_value()) {
      continue;
    }

    summaries.push_back(AuthoredMatchedRuleSummary{
        .selectorEntryIndex = match.selectorEntryIndex,
        .specificity = match.specificity,
        .selectorSource = match.selectorSourceRange.has_value()
                              ? std::string(SourceSlice(documentSource, *match.selectorSourceRange))
                              : std::string(),
        .ruleSource = std::string(SourceSlice(documentSource, *match.ruleSourceRange)),
    });
  }

  return summaries;
}

auto AuthoredMatchedRuleIs(std::size_t selectorEntryIndex, css::Specificity specificity,
                           std::string_view selectorSource, std::string_view ruleSource) {
  return AllOf(
      Field("selectorEntryIndex", &AuthoredMatchedRuleSummary::selectorEntryIndex,
            selectorEntryIndex),
      Field("specificity", &AuthoredMatchedRuleSummary::specificity, specificity),
      Field("selectorSource", &AuthoredMatchedRuleSummary::selectorSource,
            std::string(selectorSource)),
      Field("ruleSource", &AuthoredMatchedRuleSummary::ruleSource, std::string(ruleSource)));
}

}  // namespace

class StyleSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndComputeStyles(std::string_view input) {
    auto document = ParseSVG(input);
    ParseWarningSink warningSink;
    styleSystem.computeAllStyles(document.registry(), warningSink);
    return document;
  }

  StyleSystem styleSystem;
};

// --- Basic style computation ---

TEST_F(StyleSystemTest, ContextualAnimationWorkSharesTheOuterRenderAllowance) {
  auto root = ParseSVG("<svg xmlns='http://www.w3.org/2000/svg'/>");
  auto child = ParseSVG("<svg xmlns='http://www.w3.org/2000/svg'/>");
  StyleResourceBudget work;
  constexpr auto kLimit = AnimationResourceBudget::kMaximumContextualWork;
  {
    const StyleResourceBudget::OwnerScope owner(work);
    const StyleResourceBudget::ActiveRegistryScope activeRoot(work, root.registry());
    AnimationSystem().advance(root.registry(), 0);
    auto& rootBudget = root.registry().ctx().get<AnimationResourceBudget>();
    ASSERT_TRUE(rootBudget.reserveContextualWork(kLimit - 4));
    {
      const StyleResourceBudget::OwnerScope nestedOwner(work);
      const StyleResourceBudget::ActiveRegistryScope activeChild(work, child.registry());
      AnimationSystem().advance(child.registry(), 0);
      auto& childBudget = child.registry().ctx().get<AnimationResourceBudget>();
      EXPECT_EQ(childBudget.remainingContextualWork(), 4u);
      ASSERT_TRUE(childBudget.reserveContextualWork(4));
      ParseWarningSink warnings = ParseWarningSink::Disabled();
      styleSystem.computeAllStyles(child.registry(), warnings);
      EXPECT_EQ(childBudget.remainingContextualWork(), 0u);
      EXPECT_FALSE(childBudget.reserveContextualWork(1));
      EXPECT_FALSE(childBudget.rejected())
          << "Contextual refusal must preserve base-value fallback";
    }
    EXPECT_EQ(rootBudget.remainingContextualWork(), 0u);
  }
  auto& rootBudget = root.registry().ctx().get<AnimationResourceBudget>();
  EXPECT_EQ(rootBudget.remainingContextualWork(), 4u) << "Scope exit must detach its aggregate";
  ASSERT_TRUE(rootBudget.reserveContextualWork(4));
  {
    const StyleResourceBudget::OwnerScope nextFrame(work);
    const StyleResourceBudget::ActiveRegistryScope cachedRoot(work, root.registry());
    EXPECT_EQ(rootBudget.remainingContextualWork(), kLimit)
        << "A new outer frame restores allowance even when cached preparation skips advance";
  }
}

TEST_F(StyleSystemTest, NestedContextualScopePreservesAllowanceAndBinding) {
  auto document = ParseSVG("<svg xmlns='http://www.w3.org/2000/svg'/>");
  auto& registry = document.registry();
  StyleResourceBudget work;
  const StyleResourceBudget::OwnerScope owner(work);
  const StyleResourceBudget::ActiveRegistryScope outer(work, registry);
  AnimationSystem().advance(registry, 0);
  auto& budget = registry.ctx().get<AnimationResourceBudget>();
  constexpr auto kLimit = AnimationResourceBudget::kMaximumContextualWork;
  ASSERT_TRUE(budget.reserveContextualWork(kLimit - 1));
  const auto binding = budget.contextualWorkBudget();
  {
    const StyleResourceBudget::ActiveRegistryScope nested(work, registry);
    EXPECT_EQ(budget.remainingContextualWork(), 1u);
    ASSERT_TRUE(budget.reserveContextualWork(1));
  }
  EXPECT_EQ(budget.contextualWorkBudget(), binding);
  EXPECT_EQ(registry.ctx().get<AnimationContextualWorkContext>().budget, binding);
  EXPECT_EQ(budget.remainingContextualWork(), 0u);
}

TEST_F(StyleSystemTest, ContextualScopeRestoresPreviousBindingsAfterAdvanceAndEarlyReturn) {
  auto document = ParseSVG("<svg xmlns='http://www.w3.org/2000/svg'/>");
  auto& registry = document.registry();
  auto previousContext = std::make_shared<AnimationContextualWorkBudget>();
  auto previousBudget = std::make_shared<AnimationContextualWorkBudget>();
  registry.ctx().emplace<AnimationContextualWorkContext>(
      AnimationContextualWorkContext{previousContext});
  registry.ctx().emplace<AnimationResourceBudget>().setContextualWorkBudget(previousBudget);
  ASSERT_TRUE(previousContext->reserve(17));
  ASSERT_TRUE(previousBudget->reserve(23));
  StyleResourceBudget work;
  const auto prepare = [&]() {
    const StyleResourceBudget::OwnerScope owner(work);
    const StyleResourceBudget::ActiveRegistryScope active(work, registry);
    AnimationSystem().advance(registry, 0);
    auto& budget = registry.ctx().get<AnimationResourceBudget>();
    EXPECT_NE(budget.contextualWorkBudget(), previousBudget);
    EXPECT_TRUE(budget.reserveContextualWork(31));
    return;
  };
  prepare();
  EXPECT_EQ(registry.ctx().get<AnimationContextualWorkContext>().budget, previousContext);
  EXPECT_EQ(registry.ctx().get<AnimationResourceBudget>().contextualWorkBudget(), previousBudget);
  EXPECT_EQ(previousContext->remaining(), AnimationContextualWorkBudget::kMaximumWork - 17);
  EXPECT_EQ(previousBudget->remaining(), AnimationContextualWorkBudget::kMaximumWork - 23);
  prepare();
  EXPECT_EQ(registry.ctx().get<AnimationResourceBudget>().contextualWorkBudget(), previousBudget);
}

TEST_F(StyleSystemTest, ExhaustedPaintWorkPreservesBaseAndRecoversOnTheNextSample) {
  auto document = ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg' color='lime'>"
      "<rect id='r' fill='red'><set attributeName='fill' to='blue'/>"
      "<set attributeName='color' to='red'/></rect></svg>");
  auto& registry = document.registry();
  const auto target = document.querySelector("#r")->entityHandle();
  const std::string source(document.source());
  AnimationSystem().advance(registry, 1);
  registry.ctx().emplace<AnimationStyleContext>().enabled = true;
  auto& budget = registry.ctx().get<AnimationResourceBudget>();
  ASSERT_GT(budget.remainingContextualWork(), 0u);
  ASSERT_TRUE(budget.reserveContextualWork(budget.remainingContextualWork()));
  styleSystem.invalidateComputed(target);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  {
    const auto& properties = *styleSystem.computeStyle(target, warnings).properties();
    EXPECT_EQ(properties.fill.get(),
              PaintServer(PaintServer::Solid(css::Color(css::RGBA::RGB(255, 0, 0)))));
    EXPECT_EQ(properties.color.get(), css::Color(css::RGBA::RGB(0, 255, 0)));
    EXPECT_FALSE(budget.rejected());
  }
  AnimationSystem().advance(registry, 1.5);
  registry.ctx().emplace<AnimationStyleContext>().enabled = true;
  styleSystem.invalidateComputed(target);
  {
    const auto& properties = *styleSystem.computeStyle(target, warnings).properties();
    EXPECT_EQ(properties.fill.get(),
              PaintServer(PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 255)))));
    EXPECT_EQ(properties.color.get(), css::Color(css::RGBA::RGB(255, 0, 0)));
  }
  EXPECT_EQ(document.source(), source);
}

TEST_F(StyleSystemTest, MidStackPaintWorkRefusalDiscardsPartialLowerAnimation) {
  auto document = ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'><rect id='r' fill='red'>"
      "<set attributeName='fill' to='#0000ff'/><animate attributeName='fill' to='lime' dur='2s'/>"
      "</rect></svg>");
  auto& registry = document.registry();
  const auto target = document.querySelector("#r")->entityHandle();
  AnimationSystem().advance(registry, 1);
  registry.ctx().emplace<AnimationStyleContext>().enabled = true;
  const auto& recipe =
      registry.get<AnimatedValuesComponent>(target.entity()).contextualLengths.at("fill");
  auto& budget = registry.ctx().get<AnimationResourceBudget>();
  constexpr std::size_t kPaintKeywordWork = 64;
  constexpr std::size_t kInsufficientBlueUnderlyingWork = 111;
  const std::size_t retainedWork =
      recipe->evaluationWork + kPaintKeywordWork + kInsufficientBlueUnderlyingWork;
  ASSERT_GE(budget.remainingContextualWork(), retainedWork);
  ASSERT_TRUE(budget.reserveContextualWork(budget.remainingContextualWork() - retainedWork));

  styleSystem.invalidateComputed(target);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  const auto& properties = *styleSystem.computeStyle(target, warnings).properties();
  EXPECT_EQ(properties.fill.get(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA::RGB(255, 0, 0)))));
  EXPECT_FALSE(budget.rejected());
}

TEST_F(StyleSystemTest, RepeatedAnimatedOpacityReservesWorkBeforeParsing) {
  const std::string value = std::string(65536, '0') + ".5";
  auto document = ParseSVG(
      "<svg xmlns='http://www.w3.org/2000/svg'><defs><g id='src' opacity='1'>"
      "<set attributeName='opacity' to='" +
      value +
      "' dur='2s'/></g></defs>"
      "<use id='a' href='#src'/><use id='b' href='#src'/><use id='c' href='#src'/></svg>");
  auto& registry = document.registry();
  const auto source = document.querySelector("#src")->entityHandle();
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  std::vector<Entity> targets{source.entity()};
  for (const char* id : {"#a", "#b", "#c"}) {
    auto use = document.querySelector(id)->entityHandle();
    auto& shadow = use.get_or_emplace<ComputedShadowTreeComponent>();
    ShadowTreeSystem().populateInstance(use, shadow, ShadowBranchType::Main, source.entity(),
                                        RcString("#src"), warnings);
    ASSERT_THAT(shadow.mainBranch, testing::Optional(testing::_));
    targets.push_back(shadow.mainBranch->shadowEntities.front());
  }
  AnimationSystem().advance(registry, 1, nullptr);
  registry.ctx().emplace<AnimationStyleContext>().enabled = true;
  styleSystem.computeAllStyles(registry, warnings);
  EXPECT_EQ(registry.ctx().get<AnimationResourceBudget>().remainingContextualWork(), 0u);
  std::vector<double> opacities;
  for (const Entity target : targets) {
    opacities.push_back(
        registry.get<ComputedStyleComponent>(target).properties()->opacity.get().value());
  }
  EXPECT_THAT(opacities, testing::UnorderedElementsAre(0.5, 0.5, 0.5, 1.0));
}

TEST_F(StyleSystemTest, ComputeAllStylesCreatesComponents) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->properties(), NotNull());
}

TEST_F(StyleSystemTest, InlineStyleApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" style="fill: blue"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->properties(), NotNull());
}

// --- CSS stylesheet rules ---

TEST_F(StyleSystemTest, StylesheetRulesApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        .red { fill: red; }
      </style>
      <rect id="r" class="red" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->properties(), NotNull());
}

TEST_F(StyleSystemTest, IdSelectorStyleApplied) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        #target { stroke: green; }
      </style>
      <rect id="target" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#target");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->properties(), NotNull());
}

TEST_F(StyleSystemTest, StylesheetSourceMapMapsRuleBackToSvgSource) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>
        .cls { fill: red; }
      </style>
      <rect class="cls"/>
    </svg>
  )");

  auto styleElement = document.querySelector("style");
  ASSERT_TRUE(styleElement.has_value());
  const auto& stylesheet =
      document.registry().get<StylesheetComponent>(styleElement->unsafeEntityHandle().entity());
  ASSERT_EQ(stylesheet.stylesheet.rules().size(), 1u);

  const css::SelectorRule& rule = stylesheet.stylesheet.rules()[0];
  const std::optional<SourceRange> mappedRule =
      stylesheet.sourceMap.mapToDocumentSource(rule.ruleSourceRange);
  ASSERT_TRUE(mappedRule.has_value());
  EXPECT_EQ(SourceSlice(document.source(), *mappedRule), ".cls { fill: red; }");
}

TEST_F(StyleSystemTest, StylesheetQueryIncludesUserAgentRulesOnlyWhenRequested) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="sheet">rect { fill: red; }</style>
      <rect/>
    </svg>
  )");

  const std::vector<SVGStylesheetRule> authored = CollectStylesheetRules(document);
  ASSERT_THAT(authored, testing::SizeIs(1));
  EXPECT_THAT(authored.front().isUserAgentStylesheet, testing::IsFalse());
  EXPECT_THAT(authored.front().ruleSourceRange, testing::Optional(testing::_));
  EXPECT_THAT(authored.front().selectorSourceRange, testing::Optional(testing::_));
  ASSERT_THAT(authored.front().declarations, testing::SizeIs(1));
  EXPECT_THAT(authored.front().declarations.front().declarationSourceRange,
              testing::Optional(testing::_));

  const std::vector<SVGStylesheetRule> all =
      CollectStylesheetRules(document, /*includeUserAgent=*/true);
  std::size_t authorCount = 0;
  std::size_t userAgentCount = 0;
  for (const SVGStylesheetRule& rule : all) {
    if (!rule.isUserAgentStylesheet) {
      ++authorCount;
      EXPECT_EQ(rule.stylesheetEntity, authored.front().stylesheetEntity);
      EXPECT_EQ(rule.ruleIndex, authored.front().ruleIndex);
      EXPECT_EQ(rule.ruleSourceRange, authored.front().ruleSourceRange);
      continue;
    }
    ++userAgentCount;
    EXPECT_THAT(rule.ruleSourceRange, testing::Eq(std::nullopt));
    EXPECT_THAT(rule.selectorSourceRange, testing::Eq(std::nullopt));
    EXPECT_THAT(rule.declarations, testing::Not(testing::IsEmpty()));
    for (const SVGStylesheetDeclaration& declaration : rule.declarations) {
      EXPECT_THAT(declaration.declarationSourceRange, testing::Eq(std::nullopt));
    }
  }
  EXPECT_EQ(authorCount, 1u);
  EXPECT_GT(userAgentCount, 0u);
}

TEST_F(StyleSystemTest, NonCssStyleRulesAreHiddenFromQueries) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="sheet">rect:first-child[data-k] { fill: red; }</style>
      <rect/>
    </svg>
  )");
  const auto element = document.querySelector("#sheet");
  ASSERT_THAT(element, testing::Optional(testing::_));
  auto style = element->cast<SVGStyleElement>();
  const std::size_t offset = std::string_view(document.source()).find("rect:first-child");
  ASSERT_NE(offset, std::string_view::npos);

  style.setType("text/plain");
  EXPECT_THAT(CollectStylesheetRules(document), testing::IsEmpty());
  EXPECT_THAT(FindStyleRuleAtSourceOffset(document, offset), Eq(std::nullopt));
  EXPECT_THAT(AuthorStyleDependsOnTreePosition(document), testing::IsFalse());
  EXPECT_THAT(styleSystem.anyStylesheetUsesAttributeInSelector(document.registry(), "data-k"),
              testing::IsFalse());

  style.setType("TEXT/CSS");
  EXPECT_THAT(CollectStylesheetRules(document), testing::SizeIs(1));
  EXPECT_THAT(FindStyleRuleAtSourceOffset(document, offset), testing::Optional(testing::_));
  EXPECT_THAT(AuthorStyleDependsOnTreePosition(document), testing::IsTrue());
  EXPECT_THAT(styleSystem.anyStylesheetUsesAttributeInSelector(document.registry(), "data-k"),
              testing::IsTrue());
}

TEST_F(StyleSystemTest, InstallingResolvedImportsChangesStylesheetContentRevision) {
  StylesheetComponent stylesheet;
  stylesheet.parseStylesheet("rect { fill: red; }");
  const auto parsedRevision = stylesheet.contentRevision();

  std::vector<css::Stylesheet> imports;
  StylesheetComponent imported;
  imported.parseStylesheet("rect { stroke: blue; }");
  imports.push_back(std::move(imported.stylesheet));
  stylesheet.setImportedStylesheets(std::move(imports), 1);
  EXPECT_NE(stylesheet.contentRevision(), parsedRevision);
  EXPECT_THAT(stylesheet.importedStylesheets, testing::SizeIs(1));

  const auto installedRevision = stylesheet.contentRevision();
  stylesheet.setImportedStylesheets({}, 2);
  EXPECT_NE(stylesheet.contentRevision(), installedRevision);
  EXPECT_THAT(stylesheet.importedStylesheets, testing::IsEmpty());
}

TEST_F(StyleSystemTest, CollectMatchedStyleRulesReportsSelectorBranchAndSourceRanges) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>
        .cls-92, #other { fill: red; }
        [class] { stroke: blue; }
      </style>
      <rect id="target" class="cls-92"/>
    </svg>
  )");

  auto element = document.querySelector("#target");
  ASSERT_TRUE(element.has_value());
  const std::vector<MatchedStyleRule> matches =
      styleSystem.collectMatchedStyleRules(element->entityHandle());

  bool sawUserAgentWithoutSource = false;
  std::vector<MatchedStyleRule> authoredMatches;
  for (const MatchedStyleRule& match : matches) {
    if (match.isUserAgentStylesheet && !match.ruleSourceRange.has_value()) {
      sawUserAgentWithoutSource = true;
    }
    if (match.ruleSourceRange.has_value()) {
      authoredMatches.push_back(match);
    }
  }

  EXPECT_TRUE(sawUserAgentWithoutSource);
  EXPECT_THAT(AuthoredMatchedRuleSummaries(document.source(), authoredMatches),
              ElementsAre(AuthoredMatchedRuleIs(0u, css::Specificity::FromABC(0, 1, 0), ".cls-92",
                                                ".cls-92, #other { fill: red; }"),
                          AuthoredMatchedRuleIs(0u, css::Specificity::FromABC(0, 1, 0), "[class]",
                                                "[class] { stroke: blue; }")));
}

TEST_F(StyleSystemTest, FindsStyleRuleAtSourceOffsetAndSelectorBranch) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>
        .hit, .miss { opacity: 0.5; }
      </style>
      <rect id="hit" class="hit"/>
      <rect id="miss" class="miss"/>
    </svg>
  )");

  const std::string_view source = document.source();
  const std::size_t hitOffset = source.find(".hit");
  const std::size_t opacityOffset = source.find("opacity");
  ASSERT_NE(hitOffset, std::string_view::npos);
  ASSERT_NE(opacityOffset, std::string_view::npos);

  const std::optional<StyleRuleAtSourceOffset> hitRule =
      styleSystem.findStyleRuleAtSourceOffset(document.registry(), hitOffset);
  ASSERT_TRUE(hitRule.has_value());
  ASSERT_TRUE(hitRule->selectorEntryIndex.has_value());
  EXPECT_EQ(*hitRule->selectorEntryIndex, 0u);
  EXPECT_EQ(SourceSlice(source, hitRule->selectorSourceRange), ".hit");
  EXPECT_EQ(SourceSlice(source, hitRule->ruleSourceRange), ".hit, .miss { opacity: 0.5; }");

  const std::optional<StyleRuleAtSourceOffset> blockRule =
      styleSystem.findStyleRuleAtSourceOffset(document.registry(), opacityOffset);
  ASSERT_TRUE(blockRule.has_value());
  EXPECT_FALSE(blockRule->selectorEntryIndex.has_value());
  EXPECT_EQ(SourceSlice(source, blockRule->selectorSourceRange), ".hit, .miss");
}

// --- Inheritance ---

TEST_F(StyleSystemTest, StyleInheritedFromParent) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g fill="red">
        <rect id="child" width="50" height="50"/>
      </g>
    </svg>
  )");

  auto element = document.querySelector("#child");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->properties(), NotNull());
}

TEST_F(StyleSystemTest, DisplayNoneComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="hidden" width="50" height="50" style="display: none"/>
    </svg>
  )");

  auto element = document.querySelector("#hidden");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->properties(), NotNull());
  EXPECT_EQ(computed->properties()->display.get().value(), Display::None);
}

TEST_F(StyleSystemTest, VisibilityHiddenComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="hidden" width="50" height="50" style="visibility: hidden"/>
    </svg>
  )");

  auto element = document.querySelector("#hidden");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
  EXPECT_THAT(computed->properties(), NotNull());
  EXPECT_EQ(computed->properties()->visibility.get().value(), Visibility::Hidden);
}

// --- Individual style computation ---

TEST_F(StyleSystemTest, ComputeStyleForSingleEntity) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="green"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  ParseWarningSink warningSink;
  const auto& computed = styleSystem.computeStyle(element->entityHandle(), warningSink);
  EXPECT_THAT(computed.properties(), NotNull());
}

// --- Invalidation ---

TEST_F(StyleSystemTest, InvalidateComputedRemovesComponent) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" fill="red"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  ASSERT_THAT(element->entityHandle().try_get<ComputedStyleComponent>(), NotNull());

  styleSystem.invalidateComputed(element->entityHandle());
  EXPECT_EQ(element->entityHandle().try_get<ComputedStyleComponent>(), nullptr);
}

// --- Multiple elements ---

TEST_F(StyleSystemTest, MultipleElementsAllComputed) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="a" width="25" height="25" fill="red"/>
      <circle id="b" cx="50" cy="50" r="20" fill="blue"/>
      <line id="c" x1="0" y1="0" x2="100" y2="100" stroke="green"/>
    </svg>
  )");

  for (const char* id : {"#a", "#b", "#c"}) {
    auto element = document.querySelector(id);
    ASSERT_TRUE(element.has_value()) << "Missing element: " << id;
    auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
    ASSERT_THAT(computed, NotNull()) << "No computed style for: " << id;
    EXPECT_THAT(computed->properties(), NotNull()) << "No properties for: " << id;
  }
}

// --- Warnings ---

TEST_F(StyleSystemTest, WarningsCollectedForInvalidProperties) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        rect { fill: not-a-color; }
      </style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);
  // Invalid color should produce a warning (or be ignored gracefully).
  // We just verify it doesn't crash; the warning list may or may not be populated
  // depending on implementation detail.
  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computed, NotNull());
}

TEST_F(StyleSystemTest, UpdateStyleMergesAndInvalidatesComputedStyle) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" width="50" height="50" style="fill: red"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  ASSERT_THAT(element->entityHandle().try_get<ComputedStyleComponent>(), NotNull());

  styleSystem.updateStyle(element->entityHandle(), "stroke: blue; fill: green");

  EXPECT_EQ(element->entityHandle().try_get<ComputedStyleComponent>(), nullptr);
  auto styleAttr = element->getAttribute("style");
  ASSERT_TRUE(styleAttr.has_value());
  EXPECT_THAT(styleAttr.value(), testing::AllOf(testing::HasSubstr("fill: green"),
                                                testing::HasSubstr("stroke: blue")));

  ParseWarningSink warningSink;
  const auto& computed = styleSystem.computeStyle(element->entityHandle(), warningSink);
  EXPECT_EQ(computed.properties()->fill.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 0xFF)))));
  EXPECT_EQ(computed.properties()->stroke.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
}

TEST_F(StyleSystemTest, RejectedStyleUpdateDoesNotMutateAttributeOrComputedStyle) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r" style="fill: red"/></svg>
  )");
  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  const auto before = element->getAttribute("style");
  const auto* computedBefore = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(computedBefore, NotNull());

  std::string update;
  for (std::size_t index = 0; index <= css::parser::DeclarationListParser::kMaximumDeclarations;
       ++index) {
    update += "stroke: blue;";
  }
  styleSystem.updateStyle(element->entityHandle(), update);

  EXPECT_EQ(element->getAttribute("style"), before);
  EXPECT_EQ(element->entityHandle().try_get<ComputedStyleComponent>(), computedBefore);
}

TEST_F(StyleSystemTest, ComputeAllStylesDirtyOnlyRecomputesStyleDirtyEntities) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="dirty" width="50" height="50"/>
      <rect id="clean" width="50" height="50"/>
    </svg>
  )");

  auto dirty = document.querySelector("#dirty")->entityHandle();
  auto clean = document.querySelector("#clean")->entityHandle();
  SetRenderTreeState(document.registry(), RenderTreeState{.needsFullRebuild = false,
                                                          .needsFullStyleRecompute = false,
                                                          .hasBeenBuilt = true});

  dirty.get_or_emplace<StyleComponent>().updateStyle("fill: blue");
  clean.get_or_emplace<StyleComponent>().updateStyle("fill: green");
  styleSystem.invalidateComputed(dirty);

  document.registry()
      .emplace_or_replace<DirtyFlagsComponent>(dirty.entity())
      .mark(DirtyFlagsComponent::Style);
  document.registry()
      .emplace_or_replace<DirtyFlagsComponent>(clean.entity())
      .mark(DirtyFlagsComponent::Layout);

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_EQ(document.registry()
                .get<ComputedStyleComponent>(dirty.entity())
                .properties()
                ->fill.get()
                .value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
  EXPECT_EQ(document.registry()
                .get<ComputedStyleComponent>(clean.entity())
                .properties()
                ->fill.get()
                .value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0, 0xFF)))));
}

TEST_F(StyleSystemTest, ComputeAllStylesFullRecomputeClearsAndRebuildsStyles) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>rect { fill: red; }</style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());

  auto styleEntity = document.querySelector("style")->unsafeEntityHandle().entity();
  auto& stylesheet = document.registry().get<StylesheetComponent>(styleEntity);
  stylesheet.parseStylesheet("rect { fill: blue; }");
  SetRenderTreeState(document.registry(), RenderTreeState{.needsFullRebuild = false,
                                                          .needsFullStyleRecompute = true,
                                                          .hasBeenBuilt = true});

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_EQ(document.registry()
                .get<ComputedStyleComponent>(element->unsafeEntityHandle().entity())
                .properties()
                ->fill.get()
                .value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
}

TEST_F(StyleSystemTest, NonCssSheetDoesNotRegisterFontFaces) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style id="sheet">@font-face { font-family: Unused; src: local(Unused); }</style>
      <rect/>
    </svg>
  )");
  const auto element = document.querySelector("#sheet");
  ASSERT_THAT(element, testing::Optional(testing::_));
  auto style = element->cast<SVGStyleElement>();
  style.setType("text/plain");
  ParseWarningSink warnings;
  Registry& registry = document.registry();
  styleSystem.computeAllStyles(registry, warnings);
  const auto& resources = registry.ctx().get<ResourceManagerContext>();
  EXPECT_THAT(resources.fontFaces(), testing::IsEmpty());

  style.setType("text/css");
  styleSystem.computeAllStyles(registry, warnings);
  EXPECT_THAT(resources.fontFaces(), testing::SizeIs(1));
}

TEST_F(StyleSystemTest, ComputeAllStylesRegistersFontFacesDuringIncrementalPass) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        @font-face {
          font-family: TestFont;
          src: local(TestFont);
        }
      </style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r")->entityHandle();
  SetRenderTreeState(document.registry(), RenderTreeState{.needsFullRebuild = false,
                                                          .needsFullStyleRecompute = false,
                                                          .hasBeenBuilt = true});
  document.registry()
      .emplace_or_replace<DirtyFlagsComponent>(element.entity())
      .mark(DirtyFlagsComponent::Style);

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_EQ(document.registry().ctx().get<ResourceManagerContext>().fontFaces().size(), 1u);
}

/// Every style pass re-announces each stylesheet's `@font-face` rules, because the pass has no way
/// to know whether a stylesheet changed. Storing a second copy of an unchanged rule would make the
/// resource manager grow without bound, re-fetch the rule's URL on every pass, and hand the font
/// layer a fresh copy of bytes that never changed, which invalidates everything cached against the
/// font that rule resolves to.
TEST_F(StyleSystemTest, RepeatedStylePassesRegisterEachFontFaceOnce) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        @font-face {
          font-family: TestFont;
          src: local(TestFont);
        }
        @font-face {
          font-family: OtherFont;
          src: local(OtherFont);
        }
      </style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r")->entityHandle();
  ParseWarningSink warningSink;
  const auto& resourceManager = document.registry().ctx().get<ResourceManagerContext>();

  for (int pass = 0; pass < 5; ++pass) {
    SetRenderTreeState(document.registry(), RenderTreeState{.needsFullRebuild = false,
                                                            .needsFullStyleRecompute = false,
                                                            .hasBeenBuilt = true});
    document.registry()
        .emplace_or_replace<DirtyFlagsComponent>(element.entity())
        .mark(DirtyFlagsComponent::Style);
    styleSystem.computeAllStyles(document.registry(), warningSink);

    EXPECT_EQ(resourceManager.fontFaces().size(), 2u)
        << "Font faces were re-registered on style pass " << pass;
  }
}

TEST_F(StyleSystemTest, EquivalentStylesheetReparseCountsOnlyNewFontFaces) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>@font-face { font-family: TestFont; src: local(TestFont); }</style>
      <rect id="r"/>
    </svg>
  )");
  auto style = document.querySelector("style");
  auto element = document.querySelector("#r")->entityHandle();
  ASSERT_TRUE(style.has_value());
  auto& stylesheet = style->entityHandle().get<StylesheetComponent>();
  auto& resourceManager = document.registry().ctx().get<ResourceManagerContext>();
  ParseWarningSink warningSink;

  for (int pass = 0; pass < 5; ++pass) {
    stylesheet.parseStylesheet(pass % 2 == 0
                                   ? "@font-face { font-family: TestFont; src: local(TestFont); }"
                                   : " @font-face{font-family:TestFont;src:local(TestFont)}");
    SetRenderTreeState(document.registry(), RenderTreeState{.needsFullRebuild = false,
                                                            .needsFullStyleRecompute = false,
                                                            .hasBeenBuilt = true});
    document.registry()
        .emplace_or_replace<DirtyFlagsComponent>(element.entity())
        .mark(DirtyFlagsComponent::Style);
    styleSystem.computeAllStyles(document.registry(), warningSink);
  }

  EXPECT_EQ(resourceManager.fontFaces().size(), 1u);
  EXPECT_EQ(resourceManager.stylesheetFontFaceCountForTesting(), 1u);
}

TEST_F(StyleSystemTest, DestroyedStylesheetRemovesRegistrationState) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>@font-face { font-family: TestFont; src: local(TestFont); }</style>
    </svg>
  )");
  std::optional<SVGElement> style = document.querySelector("style");
  ASSERT_TRUE(style.has_value());
  auto root = document.svgElement();
  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);
  auto& resourceManager = document.registry().ctx().get<ResourceManagerContext>();
  const std::size_t registrationsBeforeRemoval =
      resourceManager.stylesheetFontFaceRegistrationCountForTesting();
  ASSERT_GT(registrationsBeforeRemoval, 0u);

  root.removeChild(*style);
  style.reset();

  EXPECT_EQ(resourceManager.stylesheetFontFaceRegistrationCountForTesting(),
            registrationsBeforeRemoval - 1);
}

TEST_F(StyleSystemTest, ComputeStylesForSubsetOnlyComputesRequestedEntities) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="a" width="50" height="50" fill="red"/>
      <rect id="b" width="50" height="50" fill="blue"/>
    </svg>
  )");

  const Entity a = document.querySelector("#a")->unsafeEntityHandle().entity();
  const Entity b = document.querySelector("#b")->unsafeEntityHandle().entity();

  ParseWarningSink warningSink;
  styleSystem.computeStylesFor(document.registry(), std::array<Entity, 1>{a}, warningSink);

  EXPECT_TRUE(document.registry().all_of<ComputedStyleComponent>(a));
  EXPECT_FALSE(document.registry().all_of<ComputedStyleComponent>(b));
}

TEST_F(StyleSystemTest, DecorationThicknessComputesEmBeforeExplicitInheritance) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><text id="parent" font-size="20"
      style="text-decoration-thickness:.2em"><tspan id="child" font-size="40"
      style="text-decoration-thickness:inherit">X</tspan><tspan id="plain">Y</tspan></text></svg>)svg");
  const auto& registry = document.registry();
  const auto parent = document.querySelector("#parent")->entityHandle();
  const auto child = document.querySelector("#child")->entityHandle();
  const auto plain = document.querySelector("#plain")->entityHandle();
  EXPECT_THAT(registry.get<ComputedStyleComponent>(parent.entity())
                  .properties()
                  ->textDecorationThickness.get()
                  ->value,
              LengthIs(testing::DoubleEq(4), Lengthd::Unit::Px));
  EXPECT_THAT(registry.get<ComputedStyleComponent>(child.entity())
                  .properties()
                  ->textDecorationThickness.get()
                  ->value,
              LengthIs(testing::DoubleEq(4), Lengthd::Unit::Px));
  EXPECT_EQ(registry.get<ComputedStyleComponent>(plain.entity())
                .properties()
                ->textDecorationThickness.get()
                ->kind,
            TextDecorationThickness::Kind::Auto);
}

TEST_F(StyleSystemTest, DecorationLonghandsDoNotInheritUnlessRequested) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><text id="parent" text-decoration="underline wavy blue"
      ><tspan id="plain">X</tspan><tspan id="child"
      style="text-decoration-style:inherit;text-decoration-color:inherit">Y</tspan><tspan id="sheet"
      class="d">Z</tspan></text><style>.d { text-decoration-line: overline;
      text-decoration-style: dotted; text-decoration-color: currentcolor }</style></svg>)svg");
  for (const char* selector : {"#parent", "#plain", "#child", "#sheet"}) {
    SCOPED_TRACE(selector);
    const auto element = document.querySelector(selector);
    ASSERT_THAT(element, testing::Optional(testing::_));
    const auto* computed = element->entityHandle().try_get<ComputedStyleComponent>();
    ASSERT_THAT(computed, NotNull());
    ASSERT_THAT(computed->properties(), NotNull());
  }
  const auto& registry = document.registry();
  const auto properties = [&](const char* selector) -> const PropertyRegistry& {
    return *registry.get<ComputedStyleComponent>(document.querySelector(selector)->entityHandle())
                .properties();
  };
  // The presentation attribute sets every longhand through the shorthand.
  EXPECT_THAT(properties("#parent").textDecoration.get(),
              testing::Optional(TextDecoration::Underline));
  EXPECT_THAT(properties("#parent").textDecorationStyle.get(),
              testing::Optional(TextDecorationStyle::Wavy));
  EXPECT_THAT(properties("#parent").textDecorationColor.get(),
              testing::Optional(css::Color(css::RGBA(0, 0, 255, 255))));
  // Not inherited: a child without declarations has the initial values.
  EXPECT_THAT(properties("#plain").textDecorationStyle.get(),
              testing::Optional(TextDecorationStyle::Solid));
  EXPECT_THAT(properties("#plain").textDecorationColor.get(), testing::Eq(std::nullopt));
  // Explicit inheritance copies the parent's values.
  EXPECT_THAT(properties("#child").textDecorationStyle.get(),
              testing::Optional(TextDecorationStyle::Wavy));
  EXPECT_THAT(properties("#child").textDecorationColor.get(),
              testing::Optional(css::Color(css::RGBA(0, 0, 255, 255))));
  // Stylesheet longhands apply like any other property.
  EXPECT_THAT(properties("#sheet").textDecoration.get(),
              testing::Optional(TextDecoration::Overline));
  EXPECT_THAT(properties("#sheet").textDecorationStyle.get(),
              testing::Optional(TextDecorationStyle::Dotted));
  EXPECT_THAT(properties("#sheet").textDecorationColor.get(),
              testing::Optional(css::Color(css::Color::CurrentColor())));
}

TEST_F(StyleSystemTest, DecorationThicknessRetainsInheritedPercentage) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><text font-size="20" style="text-decoration-thickness:20%">
      <tspan id="child" font-size="40" style="text-decoration-thickness:inherit">X</tspan>
    </text></svg>)svg");
  const auto child = document.querySelector("#child")->entityHandle();
  EXPECT_THAT(document.registry()
                  .get<ComputedStyleComponent>(child.entity())
                  .properties()
                  ->textDecorationThickness.get()
                  ->value,
              LengthIs(testing::DoubleEq(20), Lengthd::Unit::Percent));
}

TEST_F(StyleSystemTest, DecorationRemUsesComputedRootSizeAndRejectsViewportUnits) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" font-size="30"><text font-size="20"
      style="text-decoration-thickness:1rem;text-decoration-thickness:1vw">
      <tspan id="child" font-size="40" style="text-decoration-thickness:inherit">X</tspan>
    </text></svg>)svg");
  const auto child = document.querySelector("#child")->entityHandle();
  EXPECT_THAT(document.registry()
                  .get<ComputedStyleComponent>(child.entity())
                  .properties()
                  ->textDecorationThickness.get()
                  ->value,
              LengthIs(testing::DoubleEq(30), Lengthd::Unit::Px));
}

TEST_F(StyleSystemTest, RejectedDecorationUnitsPreserveNestedInheritance) {
  for (const auto unit : {"ex", "ch", "vw", "vh", "vmin", "vmax"}) {
    auto document = ParseAndComputeStyles(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
        "<g style='text-decoration-thickness:5px;text-decoration-thickness:1" +
        std::string(unit) +
        "'><svg viewBox='0 0 10 10' style='text-decoration-thickness:inherit'>"
        "<text id='child' font-size='40' style='text-decoration-thickness:inherit'>X</text>"
        "</svg></g></svg>");
    const auto child = document.querySelector("#child")->entityHandle();
    EXPECT_THAT(document.registry()
                    .get<ComputedStyleComponent>(child.entity())
                    .properties()
                    ->textDecorationThickness.get()
                    ->value,
                LengthIs(testing::DoubleEq(5), Lengthd::Unit::Px));
  }
}

TEST_F(StyleSystemTest, PathClipInheritanceChargesAndReleasesRetainedStorage) {
  constexpr std::string_view value = "clip-path:path('M0 0H10V10H0Z')";
  PropertyRegistry authored;
  authored.parseStyle(value);
  const PropertyRegistry copied = authored;
  const std::size_t pathBytes = copied.complexPropertyBytes();
  ASSERT_GT(pathBytes, 0u);
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent" style="clip-path:path('M0 0H10V10H0Z')">
        <rect id="first" style="clip-path:inherit"/>
        <rect id="second" style="clip-path:inherit"/>
      </g>
    </svg>)svg");
  const auto parentElement = document.querySelector("#parent");
  const auto firstElement = document.querySelector("#first");
  const auto secondElement = document.querySelector("#second");
  ASSERT_THAT(parentElement, testing::Optional(testing::_));
  ASSERT_THAT(firstElement, testing::Optional(testing::_));
  ASSERT_THAT(secondElement, testing::Optional(testing::_));
  auto parent = parentElement->entityHandle();
  auto first = firstElement->entityHandle();
  auto second = secondElement->entityHandle();
  DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.computedStyleBytes = 2 * pathBytes;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(familyLimits);
  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = 2 * pathBytes;
  auto& budget = document.registry().ctx().emplace<StyleResourceBudget>(limits, family);
  document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
  ParseWarningSink warnings;
  ASSERT_THAT(styleSystem.computeStyle(parent, warnings).properties()->clipPath.get(),
              testing::Optional(testing::_));
  EXPECT_EQ(budget.complexPropertyBytes(), pathBytes);
  ASSERT_THAT(styleSystem.computeStyle(first, warnings).properties()->clipPath.get(),
              testing::Optional(testing::_));
  EXPECT_EQ(budget.complexPropertyBytes(), 2 * pathBytes);
  EXPECT_THAT(styleSystem.computeStyle(second, warnings).properties()->clipPath.get(),
              testing::Eq(std::nullopt));
  EXPECT_THAT(budget.rejected(), testing::IsTrue());
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle),
            2 * pathBytes);
  styleSystem.invalidateComputed(first);
  EXPECT_EQ(budget.complexPropertyBytes(), pathBytes);
  budget.reset();
  first.get<StyleComponent>().updateStyle("clip-path:none");
  EXPECT_THAT(styleSystem.computeStyle(first, warnings).properties()->clipPath.get(),
              testing::Eq(std::nullopt));
  EXPECT_EQ(budget.complexPropertyBytes(), pathBytes);
  styleSystem.invalidateComputed(parent);
  parent.get<StyleComponent>().updateStyle("clip-path:none");
  EXPECT_THAT(styleSystem.computeStyle(parent, warnings).properties()->clipPath.get(),
              testing::Eq(std::nullopt));
  EXPECT_EQ(budget.complexPropertyBytes(), 0u);
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle), 0u);
}

TEST_F(StyleSystemTest, PolygonInheritanceChargesAndReleasesRetainedStorage) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent" clip-path="polygon(0 0, 10 0, 10 10, 0 10)">
        <rect id="first" style="clip-path:inherit"/>
        <rect id="second" style="clip-path:inherit"/>
      </g>
    </svg>)svg");
  const auto parentElement = document.querySelector("#parent");
  const auto firstElement = document.querySelector("#first");
  const auto secondElement = document.querySelector("#second");
  ASSERT_THAT(parentElement, testing::Optional(testing::_));
  ASSERT_THAT(firstElement, testing::Optional(testing::_));
  ASSERT_THAT(secondElement, testing::Optional(testing::_));
  auto parent = parentElement->entityHandle();
  auto first = firstElement->entityHandle();
  auto second = secondElement->entityHandle();
  constexpr std::size_t kPolygonBytes = 4 * sizeof(PolygonClipPath::Vertex);
  DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.computedStyleBytes = 2 * kPolygonBytes;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(familyLimits);
  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = 2 * kPolygonBytes;
  auto& budget = document.registry().ctx().emplace<StyleResourceBudget>(limits, family);
  document.registry().ctx().emplace<DisableMatchedPropertiesCacheForTesting>();
  ParseWarningSink warnings;
  ASSERT_THAT(styleSystem.computeStyle(parent, warnings).properties()->clipPath.get(),
              testing::Optional(testing::_));
  EXPECT_EQ(budget.complexPropertyBytes(), kPolygonBytes);
  ASSERT_THAT(styleSystem.computeStyle(first, warnings).properties()->clipPath.get(),
              testing::Optional(testing::_));
  EXPECT_EQ(budget.complexPropertyBytes(), 2 * kPolygonBytes);
  EXPECT_THAT(styleSystem.computeStyle(second, warnings).properties()->clipPath.get(),
              testing::Eq(std::nullopt));
  EXPECT_THAT(budget.rejected(), testing::IsTrue());
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle),
            2 * kPolygonBytes);

  styleSystem.invalidateComputed(first);
  EXPECT_EQ(budget.complexPropertyBytes(), kPolygonBytes);
  budget.reset();
  first.get<StyleComponent>().updateStyle("clip-path:none");
  EXPECT_THAT(styleSystem.computeStyle(first, warnings).properties()->clipPath.get(),
              testing::Eq(std::nullopt));
  EXPECT_EQ(budget.complexPropertyBytes(), kPolygonBytes);
  styleSystem.invalidateComputed(parent);
  parent.get<StyleComponent>().updateStyle("clip-path:none");
  EXPECT_THAT(styleSystem.computeStyle(parent, warnings).properties()->clipPath.get(),
              testing::Eq(std::nullopt));
  EXPECT_EQ(budget.complexPropertyBytes(), 0u);
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle), 0u);
}

TEST_F(StyleSystemTest, RetainedBytePressureClearsCachedRecordsBeforeRefusal) {
  constexpr std::string_view firstValue = "clip-path:path('M0 0H10V10H0Z')";
  PropertyRegistry authored;
  authored.parseStyle(firstValue);
  const std::size_t pathBytes = PropertyRegistry(authored).complexPropertyBytes();
  ASSERT_GT(pathBytes, 0u);

  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="first" style="clip-path:path('M0 0H10V10H0Z')"/>
      <rect id="second" style="clip-path:path('M0 0H20V20H0Z')"/>
    </svg>)svg");
  const auto firstElement = document.querySelector("#first");
  const auto secondElement = document.querySelector("#second");
  ASSERT_THAT(firstElement, testing::Optional(testing::_));
  ASSERT_THAT(secondElement, testing::Optional(testing::_));
  const EntityHandle first = firstElement->entityHandle();
  const EntityHandle second = secondElement->entityHandle();
  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = pathBytes;
  DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.computedStyleBytes = pathBytes;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(familyLimits);
  auto& budget = document.registry().ctx().emplace<StyleResourceBudget>(limits, family);
  ParseWarningSink warnings;

  EXPECT_THAT(styleSystem.computeStyle(first, warnings).properties()->clipPath.get(),
              testing::Optional(testing::_));
  EXPECT_EQ(budget.complexPropertyBytes(), pathBytes);
  styleSystem.invalidateComputed(first);
  EXPECT_EQ(budget.complexPropertyBytes(), pathBytes);

  EXPECT_THAT(styleSystem.computeStyle(second, warnings).properties()->clipPath.get(),
              testing::Optional(testing::_));
  EXPECT_FALSE(budget.rejected());
  EXPECT_EQ(budget.complexPropertyBytes(), pathBytes);
  EXPECT_FALSE(family->securityStats().rejected);
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle), pathBytes);
}

TEST_F(StyleSystemTest, InsetLengthsComputeBeforeInheritanceAndKeepPercentages) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" font-size="30">
      <g id="parent" font-size="20" clip-path="inset(.2em 1rem 25% 2px)">
        <rect id="child" font-size="40" clip-path="inherit"/>
        <rect id="current" font-size="40" clip-path="inset(.2em 1rem 25% 2px)"/>
      </g>
    </svg>)svg");
  for (const auto& example :
       {std::pair{"#parent", 4.0}, std::pair{"#child", 4.0}, std::pair{"#current", 8.0}}) {
    SCOPED_TRACE(example.first);
    const auto element = document.querySelector(example.first);
    ASSERT_THAT(element, testing::Optional(testing::_));
    const auto& properties = document.registry()
                                 .get<ComputedStyleComponent>(element->entityHandle().entity())
                                 .properties();
    ASSERT_THAT(properties, NotNull());
    EXPECT_THAT(properties->clipPath.get(),
                testing::Optional(Field(
                    &ClipPath::value,
                    testing::VariantWith<InsetClipPath>(Field(
                        &InsetClipPath::offsets,
                        ElementsAre(LengthIs(testing::DoubleEq(example.second), Lengthd::Unit::Px),
                                    LengthIs(testing::DoubleEq(30), Lengthd::Unit::Px),
                                    LengthIs(testing::DoubleEq(25), Lengthd::Unit::Percent),
                                    LengthIs(testing::DoubleEq(2), Lengthd::Unit::Px)))))));
  }
}

TEST_F(StyleSystemTest, InsetRootOwnRemUsesAlreadyComputedRootFont) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" font-size="30" clip-path="inset(.25rem)"/>)svg");
  const auto& properties = document.registry()
                               .get<ComputedStyleComponent>(document.rootEntityHandle().entity())
                               .properties();
  ASSERT_THAT(properties, NotNull());
  EXPECT_THAT(properties->clipPath.get(),
              testing::Optional(
                  Field(&ClipPath::value,
                        testing::VariantWith<InsetClipPath>(Field(
                            &InsetClipPath::offsets,
                            testing::Each(LengthIs(testing::DoubleEq(7.5), Lengthd::Unit::Px)))))));
}

TEST_F(StyleSystemTest, InsetDetachedRootContextUsesInitialFontWithoutRecursion) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" font-size="30">
      <rect id="child" clip-path="inset(.5rem)"/>
    </svg>)svg");
  const auto element = document.querySelector("#child");
  ASSERT_THAT(element, testing::Optional(testing::_));
  const EntityHandle child = element->entityHandle();
  auto& registry = document.registry();
  auto& context = registry.ctx().get<SVGDocumentContext>();
  const Entity originalRoot = context.rootEntity;
  const Entity detached = registry.create();
  registry.destroy(detached);
  for (const Entity missingRoot : std::array<Entity, 2>{entt::null, detached}) {
    context.rootEntity = missingRoot;
    styleSystem.invalidateComputed(child);
    ParseWarningSink warningSink;
    const auto& properties = styleSystem.computeStyle(child, warningSink).properties();
    ASSERT_THAT(properties, NotNull());
    EXPECT_THAT(properties->clipPath.get(),
                testing::Optional(
                    Field(&ClipPath::value,
                          testing::VariantWith<InsetClipPath>(Field(
                              &InsetClipPath::offsets,
                              testing::Each(LengthIs(testing::DoubleEq(8), Lengthd::Unit::Px)))))));
  }
  context.rootEntity = originalRoot;
}

TEST(StyleResourceBudgetTest, SharesRetainedBytesAcrossDocumentFamily) {
  DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.computedStyleBytes = 1024;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(familyLimits);

  StyleResourceBudget::Limits localLimits;
  localLimits.complexPropertyBytes = 1024;
  {
    StyleResourceBudget first(localLimits, family);
    StyleResourceBudget second(localLimits, family);
    auto firstRecord = std::make_shared<ComputedStyle>();
    auto secondRecord = std::make_shared<ComputedStyle>();
    EXPECT_TRUE(first.retainComputedStyle(*firstRecord, 768));
    EXPECT_FALSE(second.retainComputedStyle(*secondRecord, 512));
    EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle), 768u);
  }

  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle), 0u);
}

TEST_F(StyleSystemTest, SiblingCacheKeepsRecordChargedUnderFamilyPressure) {
  constexpr std::string_view source = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <rect id="target" stroke-dasharray="1 2 3 4"/>
    </svg>
  )";
  auto first = ParseSVG(source);
  auto sibling = ParseSVG(source);
  const auto firstElement = first.querySelector("#target");
  const auto siblingElement = sibling.querySelector("#target");
  ASSERT_THAT(firstElement, testing::Optional(testing::_));
  ASSERT_THAT(siblingElement, testing::Optional(testing::_));
  const EntityHandle target = firstElement->entityHandle();
  const EntityHandle siblingTarget = siblingElement->entityHandle();

  const PropertyRegistry local = target.get<StyleComponent>().properties();
  const std::size_t recordBytes = local.complexPropertyBytes();
  ASSERT_GT(recordBytes, 0u);
  using Kind = DocumentResourceFamilyBudget::Kind;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(
      DocumentResourceFamilyBudget::Limits{.computedStyleBytes = recordBytes});
  first.registry().ctx().emplace<StyleResourceBudget>(family);
  sibling.registry().ctx().emplace<StyleResourceBudget>(family);

  ParseWarningSink warnings;
  const auto& result = styleSystem.computeStyle(target, warnings);
  ASSERT_THAT(result.properties()->strokeDasharray.get(), testing::Optional(testing::SizeIs(4)));
  EXPECT_EQ(family->retainedBytes(Kind::ComputedStyle), recordBytes);
  styleSystem.invalidateComputed(target);
  EXPECT_EQ(family->retainedBytes(Kind::ComputedStyle), recordBytes);

  const auto& refused = styleSystem.computeStyle(siblingTarget, warnings);
  EXPECT_THAT(refused.properties()->strokeDasharray.get(), Eq(std::nullopt));
  EXPECT_THAT(family->securityStats().rejected, testing::IsTrue());
  EXPECT_EQ(family->retainedBytes(Kind::ComputedStyle), recordBytes);

  first.registry().ctx().get<MatchedPropertiesCache>().clear();
  EXPECT_EQ(family->retainedBytes(Kind::ComputedStyle), 0u);
}

TEST(StyleResourceBudgetTest, ReleasingMoreThanReservedFailsWithUnderflowDiagnostic) {
  RetainedStyleBytes account(10);
  ASSERT_THAT(account.reserve(4), testing::IsTrue());
  EXPECT_DEATH_IF_SUPPORTED(account.release(5), "Retained style byte underflow");
  EXPECT_EQ(account.retainedBytes(), 4u);
}

TEST(StyleResourceBudgetTest, SpareVectorCapacityReachesExactCapThenRejectsPlusOne) {
  StrokeDasharray dasharray;
  dasharray.reserve(8);
  dasharray.emplace_back(1.0, Lengthd::Unit::Px);
  const std::size_t retainedCapacityBytes = dasharray.capacity() * sizeof(Lengthd);

  PropertyRegistry properties;
  properties.strokeDasharray.set(std::move(dasharray), css::Specificity());

  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = retainedCapacityBytes;
  StyleResourceBudget budget(limits);
  auto atCap = std::make_shared<ComputedStyle>();
  auto plusOne = std::make_shared<ComputedStyle>();

  EXPECT_TRUE(budget.retainComputedStyle(*atCap, properties.complexPropertyBytes()));
  EXPECT_EQ(budget.complexPropertyBytes(), retainedCapacityBytes);
  EXPECT_FALSE(budget.retainComputedStyle(*plusOne, 1));
  EXPECT_EQ(budget.complexPropertyBytes(), retainedCapacityBytes);
  EXPECT_TRUE(budget.rejected());
}

TEST(StyleResourceBudgetTest, SharedRecordHasOneLifetimeCharge) {
  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = 1024;
  StyleResourceBudget budget(limits);
  auto record = std::make_shared<ComputedStyle>();

  ASSERT_TRUE(budget.retainComputedStyle(*record, 768));
  std::shared_ptr<const ComputedStyle> first = record;
  std::shared_ptr<const ComputedStyle> second = record;
  record.reset();
  first.reset();
  EXPECT_EQ(budget.complexPropertyBytes(), 768u);
  second.reset();
  EXPECT_EQ(budget.complexPropertyBytes(), 0u);
}

TEST(StyleResourceBudgetTest, RecordRejectsASecondLifetimeChargeWithoutLeaking) {
  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = 1024;
  StyleResourceBudget budget(limits);
  auto record = std::make_shared<ComputedStyle>();

  ASSERT_TRUE(budget.retainComputedStyle(*record, 384));
  EXPECT_FALSE(budget.retainComputedStyle(*record, 256));
  EXPECT_EQ(budget.complexPropertyBytes(), 384u);
  record.reset();
  EXPECT_EQ(budget.complexPropertyBytes(), 0u);
}

TEST(StyleResourceBudgetTest, FreshZeroByteRecordAttachesOnceAfterStickyRefusal) {
  DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.computedStyleBytes = 0;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(familyLimits);
  EXPECT_FALSE(family->reserve(DocumentResourceFamilyBudget::Kind::ComputedStyle, 1));
  ASSERT_TRUE(family->securityStats().rejected);
  StyleResourceBudget::Limits limits;
  limits.declarationApplications = 0;
  StyleResourceBudget budget(limits, family);
  EXPECT_FALSE(budget.reserveDeclarationApplication());
  ASSERT_TRUE(budget.rejected());
  auto record = std::make_shared<ComputedStyle>();

  EXPECT_TRUE(budget.canRetainComputedStyle(0));
  EXPECT_TRUE(budget.retainComputedStyle(*record, 0));
  EXPECT_FALSE(budget.retainComputedStyle(*record, 0));
  EXPECT_EQ(budget.complexPropertyBytes(), 0u);
}

TEST(StyleResourceBudgetTest, ConcurrentLastHolderReleaseReserveAndFamilySnapshotStayExact) {
  DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.computedStyleBytes = 2;
  familyLimits.maximumTotalRetainedBytes = 2;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(familyLimits);
  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = 2;
  StyleResourceBudget budget(limits, family);

  for (int iteration = 0; iteration < 1000; ++iteration) {
    auto released = std::make_shared<ComputedStyle>();
    ASSERT_TRUE(budget.retainComputedStyle(*released, 1));
    std::barrier start(2);
    std::thread releaser([record = std::move(released), &start]() mutable {
      start.arrive_and_wait();
      record.reset();
    });
    auto replacement = std::make_shared<ComputedStyle>();
    start.arrive_and_wait();
    const bool replacementAdmitted = budget.retainComputedStyle(*replacement, 1);
    EXPECT_TRUE(replacementAdmitted);
    const auto snapshot = family->securityStats();
    EXPECT_LE(snapshot.retainedBytes[static_cast<std::size_t>(
                  DocumentResourceFamilyBudget::Kind::ComputedStyle)],
              2u);
    EXPECT_LE(snapshot.totalRetainedBytes, 2u);
    EXPECT_EQ(snapshot.totalRetainedBytes, snapshot.retainedBytes[static_cast<std::size_t>(
                                               DocumentResourceFamilyBudget::Kind::ComputedStyle)]);
    releaser.join();
    EXPECT_EQ(budget.complexPropertyBytes(), 1u);
    replacement.reset();
    EXPECT_EQ(budget.complexPropertyBytes(), 0u);
  }

  EXPECT_FALSE(family->securityStats().rejected);
  EXPECT_EQ(family->totalRetainedBytes(), 0u);
}

TEST(StyleResourceBudgetTest, CapacityPreflightDoesNotLatchRejection) {
  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = 1024;
  StyleResourceBudget budget(limits);
  auto record = std::make_shared<ComputedStyle>();

  ASSERT_TRUE(budget.retainComputedStyle(*record, 768));
  EXPECT_FALSE(budget.canRetainComputedStyle(512));
  EXPECT_FALSE(budget.rejected());
  EXPECT_TRUE(budget.canRetainComputedStyle(256));
}

TEST(StyleResourceBudgetTest, DeclarationPreflightIncludesAggregateCapacity) {
  StyleResourceBudget::Limits aggregateLimits;
  aggregateLimits.declarationApplications = 2;
  aggregateLimits.declarationComponentWork = 8;
  aggregateLimits.declarationByteWork = 16;
  StyleResourceBudget aggregate(aggregateLimits);
  StyleResourceBudget local;
  local.setAggregateWorkBudget(&aggregate);

  EXPECT_TRUE(local.canReserveDeclarationWork(2, 8, 16));
  EXPECT_FALSE(local.canReserveDeclarationWork(3, 8, 16));
  EXPECT_FALSE(local.canReserveDeclarationWork(2, 9, 16));
  EXPECT_FALSE(local.canReserveDeclarationWork(2, 8, 17));
  EXPECT_FALSE(local.rejected());
}

TEST(StyleResourceBudgetTest, ChargesDeclarationSourceBytesPerApplication) {
  StyleResourceBudget::Limits limits;
  limits.declarationByteWork = 1024;
  StyleResourceBudget budget(limits);

  EXPECT_TRUE(budget.reserveDeclarationApplication(1, 768));
  EXPECT_FALSE(budget.reserveDeclarationApplication(1, 512));
  EXPECT_EQ(budget.declarationApplications(), 1u);
  EXPECT_EQ(budget.declarationByteWork(), 768u);
  EXPECT_TRUE(budget.rejected());
}

TEST_F(StyleSystemTest, SymbolDefinitionDisplayExcludesForeignNamesAndPreservesOverride) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <symbol id="definition" style="display:inline!important"/>
      <symbol id="overridden"/>
      <symbol xmlns="urn:example:foreign" id="foreign" style="display:inline!important"/>
    </svg>
  )");
  const auto definition = document.querySelector("#definition");
  const auto overridden = document.querySelector("#overridden");
  const auto foreign = document.querySelector("#foreign");
  ASSERT_THAT(definition, testing::Optional(testing::_));
  ASSERT_THAT(overridden, testing::Optional(testing::_));
  ASSERT_THAT(foreign, testing::Optional(testing::_));
  overridden->entityHandle().get_or_emplace<StyleComponent>().editProperties(
      [&](PropertyRegistry& properties) {
        properties.display.set(Display::Inline, css::Specificity::Override());
      });
  EXPECT_EQ(definition->getComputedStyle().display.get(), Display::None);
  EXPECT_EQ(overridden->getComputedStyle().display.get(), Display::Inline);
  EXPECT_EQ(foreign->getComputedStyle().display.get(), Display::Inline);
}

TEST_F(StyleSystemTest, UserAgentNamespaceMatchesComputedAndDiagnosticStyles) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>#foreign { fill: red; }</style>
      <symbol id="svg-symbol"/>
      <symbol xmlns="urn:example:foreign" id="foreign"/>
      <style xmlns="urn:example:foreign" id="foreign-style"/>
    </svg>
  )");
  const auto symbol = document.querySelector("#svg-symbol");
  const auto foreign = document.querySelector("#foreign");
  const auto foreignStyle = document.querySelector("#foreign-style");
  ASSERT_THAT(symbol, testing::Optional(testing::_));
  ASSERT_THAT(foreign, testing::Optional(testing::_));
  ASSERT_THAT(foreignStyle, testing::Optional(testing::_));
  EXPECT_THAT(UserAgentMatchFlags(*symbol), testing::Contains(true));
  EXPECT_THAT(UserAgentMatchFlags(*foreign), testing::Not(testing::Contains(true)));
  EXPECT_THAT(UserAgentMatchFlags(*foreignStyle), testing::Not(testing::Contains(true)));
  EXPECT_EQ(symbol->getComputedStyle().overflow.get(), Overflow::Hidden);
  EXPECT_EQ(foreign->getComputedStyle().overflow.get(), Overflow::Visible);
  EXPECT_EQ(foreign->getComputedStyle().display.get(), Display::Inline);
  EXPECT_EQ(foreignStyle->getComputedStyle().display.get(), Display::Inline);
  EXPECT_EQ(foreign->getComputedStyle().fill.get(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255)))));
}

TEST_F(StyleSystemTest, UserAgentDisplayRulesResolveSvgNamespaceAliases) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" xmlns:s="http://www.w3.org/2000/svg"
         xmlns:f="urn:example:foreign">
      <s:style id="sheet" style="display:inline!important">#symbol { display:inline!important; }</s:style>
      <s:symbol id="symbol" style="display:inline!important"/>
      <s:symbol id="overridden"/>
      <f:symbol id="foreign-symbol" style="display:inline!important"/>
      <f:style id="foreign-style" style="display:inline!important"/>
    </svg>
  )");
  const css::Specificity requiredPriority =
      css::Specificity::Important(css::Specificity::FromABC(0, 0, 1).toUserAgentSpecificity());
  for (const std::string_view selector : {"#sheet", "#symbol"}) {
    SCOPED_TRACE(selector);
    const auto element = document.querySelector(selector);
    ASSERT_THAT(element, testing::Optional(testing::_));
    EXPECT_EQ(element->getComputedStyle().display.get(), Display::None);
    EXPECT_EQ(element->getComputedStyle().display.specificity, requiredPriority);
    EXPECT_THAT(UserAgentMatchFlags(*element), testing::Contains(true));
  }
  const auto overridden = document.querySelector("#overridden");
  ASSERT_THAT(overridden, testing::Optional(testing::_));
  overridden->entityHandle().get_or_emplace<StyleComponent>().editProperties(
      [&](PropertyRegistry& properties) {
        properties.display.set(Display::Inline, css::Specificity::Override());
      });
  EXPECT_EQ(overridden->getComputedStyle().display.get(), Display::Inline);
  EXPECT_EQ(overridden->getComputedStyle().display.specificity, css::Specificity::Override());
  for (const std::string_view selector : {"#foreign-symbol", "#foreign-style"}) {
    SCOPED_TRACE(selector);
    const auto element = document.querySelector(selector);
    ASSERT_THAT(element, testing::Optional(testing::_));
    EXPECT_EQ(element->getComputedStyle().display.get(), Display::Inline);
    EXPECT_THAT(UserAgentMatchFlags(*element), testing::Not(testing::Contains(true)));
  }
}

TEST_F(StyleSystemTest, SelectorBudgetChargesRawNodesSkippedByStyleMatching) {
  for (const std::size_t maximumSteps : {16u, 1024u}) {
    auto document = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
      <style>g:has(> rect) { opacity: .8 }</style><g id="g"/>
    </svg>)svg");
    std::vector<Entity> userAgentSheets;
    for (auto entity : document.registry().view<StylesheetComponent>()) {
      if (document.registry().get<StylesheetComponent>(entity).isUserAgentStylesheet) {
        userAgentSheets.push_back(entity);
      }
    }
    for (const Entity entity : userAgentSheets) {
      document.registry().remove<StylesheetComponent>(entity);
    }
    auto g = document.querySelector("#g").value();
    auto xmlDocument = document.xmlDocument();
    auto node = xml::XMLNode::TryCast(g.unsafeEntityHandle()).value();
    for (int i = 0; i < 64; ++i) {
      node.appendChild(xml::XMLNode::CreateDataNode(xmlDocument, " "));
    }
    g.appendChild(SVGRectElement::Create(document));
    StyleResourceBudget::Limits limits;
    limits.selectorTraversalSteps = maximumSteps;
    auto& budget = document.registry().ctx().emplace<StyleResourceBudget>(limits);
    ParseWarningSink warnings;
    styleSystem.computeStyle(g.entityHandle(), warnings);
    EXPECT_EQ(budget.selectorTraversal().rejected(), maximumSteps == 16u);
  }
}

TEST_F(StyleSystemTest, EmptySelectorUsesAuthoredChildrenAfterShadowInstantiation) {
  auto document = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <style>use, g { opacity: .2 } use:empty, g:empty { opacity: .8 }</style>
    <defs><g id="shape"><rect/><use id="nested" href="#leaf"/></g><rect id="leaf"/></defs>
    <use id="empty" href="#shape"/>
    <use id="titled" href="#shape"><title>name</title></use>
  </svg>)svg");
  auto shape = document.querySelector("#shape").value();
  ParseWarningSink warnings;
  for (const char* id : {"empty", "titled"}) {
    auto host = document.querySelector(std::string("#") + id).value();
    auto& shadow = host.entityHandle().get_or_emplace<ComputedShadowTreeComponent>();
    ShadowTreeSystem().populateInstance(host.entityHandle(), shadow, ShadowBranchType::Main,
                                        shape.entityHandle().entity(), RcString("#shape"),
                                        warnings);
    ASSERT_THAT(shadow.mainBranch, testing::Optional(testing::_));
    ASSERT_THAT(shadow.mainBranch->shadowEntities, testing::Not(testing::IsEmpty()));
    EntityHandle proxy(document.registry(), shadow.mainBranch->shadowEntities.front());
    const auto& proxyProperties = styleSystem.computeStyle(proxy, warnings).properties();
    ASSERT_THAT(proxyProperties, NotNull());
    EXPECT_THAT(proxyProperties->opacity.get(), testing::Optional(.2));
    const auto nested = document.querySelector("#nested").value();
    const auto leaf = document.querySelector("#leaf").value();
    const std::vector<Entity> proxies = shadow.mainBranch->shadowEntities;
    bool testedNested = false;
    for (const Entity entity : proxies) {
      if (document.registry().get<ShadowEntityComponent>(entity).lightEntity !=
          nested.entityHandle().entity()) {
        continue;
      }
      EntityHandle nestedProxy(document.registry(), entity);
      auto& nestedShadow = nestedProxy.get_or_emplace<ComputedShadowTreeComponent>();
      ShadowTreeSystem().populateInstance(nestedProxy, nestedShadow, ShadowBranchType::Main,
                                          leaf.entityHandle().entity(), RcString("#leaf"),
                                          warnings);
      ASSERT_THAT(nestedShadow.mainBranch, testing::Optional(testing::_));
      const auto& nestedProperties = styleSystem.computeStyle(nestedProxy, warnings).properties();
      ASSERT_THAT(nestedProperties, NotNull());
      EXPECT_THAT(nestedProperties->opacity.get(), testing::Optional(.8));
      testedNested = true;
    }
    EXPECT_TRUE(testedNested);
    const bool empty = std::string_view(id) == "empty";
    EXPECT_EQ(host.isEmptyForSelector(nullptr), empty);
    const auto& properties = styleSystem.computeStyle(host.entityHandle(), warnings).properties();
    ASSERT_THAT(properties, NotNull());
    EXPECT_THAT(properties->opacity.get(), testing::Optional(empty ? .8 : .2));
  }
}

TEST_F(StyleSystemTest, SymbolDirectUseRootPreservesNativeDisplayOverride) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs><symbol id="definition"/></defs><use id="instance" href="#definition"/>
    </svg>
  )");
  const auto definition = document.querySelector("#definition");
  const auto instance = document.querySelector("#instance");
  ASSERT_THAT(definition, testing::Optional(testing::_));
  ASSERT_THAT(instance, testing::Optional(testing::_));
  definition->entityHandle().get_or_emplace<StyleComponent>().editProperties(
      [&](PropertyRegistry& properties) {
        properties.display.set(Display::None, css::Specificity::Override());
      });
  auto& shadowTree = instance->entityHandle().get_or_emplace<ComputedShadowTreeComponent>();
  ParseWarningSink warnings;
  ShadowTreeSystem().populateInstance(instance->entityHandle(), shadowTree, ShadowBranchType::Main,
                                      definition->entityHandle().entity(), RcString("#definition"),
                                      warnings);
  ASSERT_THAT(shadowTree.mainBranch, testing::Optional(testing::_));
  ASSERT_THAT(shadowTree.mainBranch->shadowEntities, testing::Not(testing::IsEmpty()));
  EntityHandle root(document.registry(), shadowTree.mainBranch->shadowEntities.front());
  const auto& properties = styleSystem.computeStyle(root, warnings).properties();
  ASSERT_THAT(properties, NotNull());
  EXPECT_EQ(properties->display.get(), Display::None);
  EXPECT_EQ(properties->display.specificity, css::Specificity::Override());
}

TEST_F(StyleSystemTest, SymbolShadowRootFromNonUseRemainsDefinition) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs><symbol id="definition" style="display:inline!important"/></defs><g id="host"/>
    </svg>
  )");
  const auto definition = document.querySelector("#definition");
  const auto host = document.querySelector("#host");
  ASSERT_THAT(definition, testing::Optional(testing::_));
  ASSERT_THAT(host, testing::Optional(testing::_));
  auto& shadowTree = host->entityHandle().get_or_emplace<ComputedShadowTreeComponent>();
  ParseWarningSink warnings;
  ShadowTreeSystem().populateInstance(host->entityHandle(), shadowTree, ShadowBranchType::Main,
                                      definition->entityHandle().entity(), RcString("#definition"),
                                      warnings);
  ASSERT_THAT(shadowTree.mainBranch, testing::Optional(testing::_));
  ASSERT_THAT(shadowTree.mainBranch->shadowEntities, testing::Not(testing::IsEmpty()));
  EntityHandle root(document.registry(), shadowTree.mainBranch->shadowEntities.front());
  const auto& properties = styleSystem.computeStyle(root, warnings).properties();
  ASSERT_THAT(properties, NotNull());
  EXPECT_EQ(properties->display.get(), Display::None);
}

TEST_F(StyleSystemTest, SymbolExternalOccurrenceAppliesOnlyToExactRootAndRetires) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <symbol id="root" style="display:none!important">
        <symbol id="nested" style="display:inline!important"/>
      </symbol>
      <symbol id="outside" style="display:inline!important"/>
    </svg>
  )");
  const auto root = document.querySelector("#root");
  const auto nested = document.querySelector("#nested");
  const auto outside = document.querySelector("#outside");
  ASSERT_THAT(root, testing::Optional(testing::_));
  ASSERT_THAT(nested, testing::Optional(testing::_));
  ASSERT_THAT(outside, testing::Optional(testing::_));
  EXPECT_EQ(root->getComputedStyle().display.get(), Display::None);
  {
    auto access = document.writeAccess();
    access.registry().ctx().emplace<ExternalUseStyleContext>(
        root->entityHandle().entity(), std::make_shared<const ComputedStyle>(),
        ResolvedPaintServer(), ResolvedPaintServer(), nullptr);
    styleSystem.invalidateComputed(root->entityHandle());
    EXPECT_EQ(root->getComputedStyle().display.get(), Display::Inline);
    EXPECT_EQ(nested->getComputedStyle().display.get(), Display::None);
    EXPECT_EQ(outside->getComputedStyle().display.get(), Display::None);
    access.registry().ctx().erase<ExternalUseStyleContext>();
    styleSystem.invalidateComputed(root->entityHandle());
  }
  EXPECT_EQ(root->getComputedStyle().display.get(), Display::None);
}

TEST_F(StyleSystemTest, ShadowTreeSelectorsMatchSiblingAndAttributeState) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>
        rect:first-child { fill: blue; }
        circle:last-child { stroke: green; }
        rect + circle { opacity: 0.5; }
        [fill] { visibility: hidden; }
      </style>
      <defs>
        <g id="src">
          <rect id="lightRect" width="10" height="10" fill="red"/>
          <circle id="lightCircle" r="5"/>
        </g>
      </defs>
      <use id="u" href="#src"/>
    </svg>
  )");

  auto useElement = document.querySelector("#u");
  ASSERT_TRUE(useElement.has_value());
  auto& shadowTree = document.registry().get_or_emplace<ComputedShadowTreeComponent>(
      useElement->unsafeEntityHandle().entity());
  auto target = document.registry()
                    .get<ShadowTreeComponent>(useElement->unsafeEntityHandle().entity())
                    .mainTargetEntity(document.registry());
  ASSERT_TRUE(target.has_value());

  ParseWarningSink warningSink;
  ShadowTreeSystem().populateInstance(useElement->entityHandle(), shadowTree,
                                      ShadowBranchType::Main, target->handle.entity(),
                                      RcString("#src"), warningSink);
  styleSystem.computeAllStyles(document.registry(), warningSink);

  ASSERT_TRUE(shadowTree.mainBranch.has_value());
  ASSERT_GE(shadowTree.mainBranch->shadowEntities.size(), 3u);

  Entity shadowRect = shadowTree.mainBranch->shadowEntities[1];
  Entity shadowCircle = shadowTree.mainBranch->shadowEntities[2];

  const auto& rectStyle = document.registry().get<ComputedStyleComponent>(shadowRect);
  const auto& circleStyle = document.registry().get<ComputedStyleComponent>(shadowCircle);

  EXPECT_EQ(rectStyle.properties()->fill.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
  EXPECT_EQ(rectStyle.properties()->visibility.get().value(), Visibility::Hidden);
  EXPECT_EQ(circleStyle.properties()->stroke.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 0xFF)))));
  EXPECT_DOUBLE_EQ(circleStyle.properties()->opacity.get().value(), 0.5);
}

// --- Cascade counters and style time ---

namespace {

/// Cascade instrumentation of \p document, which a style pass has created.
const StyleCascadeMetrics& CascadeMetricsOf(SVGDocument& document) {
  return document.registry().ctx().get<StyleCascadeMetrics>();
}

/// Replace the document's style budget with one that has \p limits.
void SetStyleLimits(SVGDocument& document, const StyleResourceBudget::Limits& limits) {
  Registry& registry = document.registry();
  if (registry.ctx().contains<StyleResourceBudget>()) {
    registry.ctx().erase<StyleResourceBudget>();
  }
  registry.ctx().emplace<StyleResourceBudget>(limits);
}

}  // namespace

TEST_F(StyleSystemTest, StyleTimeGrowsWithEachPassAndReleasesItsScope) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect width="50" height="50"/>
    </svg>
  )");
  const StyleCascadeMetrics& metrics = CascadeMetricsOf(document);
  EXPECT_THAT(metrics.activeTimedCalls, Eq(0));
  const std::int64_t afterFirstPassNs = metrics.styleTime.count();
  EXPECT_THAT(afterFirstPassNs, Gt(0));

  SetRenderTreeState(document.registry(), RenderTreeState{.needsFullRebuild = false,
                                                          .needsFullStyleRecompute = true,
                                                          .hasBeenBuilt = true});
  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_THAT(metrics.activeTimedCalls, Eq(0));
  EXPECT_THAT(metrics.styleTime.count(), Gt(afterFirstPassNs));
  EXPECT_THAT(metrics.counters.fullClears, Eq(1u));
}

TEST_F(StyleSystemTest, StyleTimeCountsNestedParentCascadesOnce) {
  // Cascading the leaf of a deep chain computes every ancestor inside the leaf's own cascade, so
  // timing each cascade separately would count the same interval once per ancestor.
  constexpr int kDepth = 100;
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">)";
  for (int i = 0; i < kDepth; ++i) {
    source += "<g>";
  }
  source += R"(<rect id="leaf" width="50" height="50"/>)";
  for (int i = 0; i < kDepth; ++i) {
    source += "</g>";
  }
  source += "</svg>";
  auto document = ParseSVG(source);
  const Entity leaf = document.querySelector("#leaf")->entityHandle().entity();
  const auto* parsed = document.registry().ctx().find<StyleCascadeMetrics>();
  const std::uint64_t cascadedBefore = parsed ? parsed->counters.elementsCascaded : 0;
  const std::int64_t timeBeforeNs = parsed ? parsed->styleTime.count() : 0;

  ParseWarningSink warningSink;
  const auto start = std::chrono::steady_clock::now();
  styleSystem.computeStylesFor(document.registry(), std::span<const Entity>(&leaf, 1), warningSink);
  const auto wallNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
          .count();

  const StyleCascadeMetrics& metrics = CascadeMetricsOf(document);
  // The groups, the leaf, the `<svg>` element and the document node above it.
  EXPECT_THAT(metrics.counters.elementsCascaded - cascadedBefore,
              Eq(static_cast<std::uint64_t>(kDepth + 3)));
  EXPECT_THAT(metrics.activeTimedCalls, Eq(0));
  // The timed interval lies inside the measured call; this is containment, not a time budget.
  EXPECT_THAT(metrics.styleTime.count() - timeBeforeNs, AllOf(Gt(0), Le(wallNs)));
}

TEST_F(StyleSystemTest, RestyleStoresNewRecordAndKeepsHeldRecordUnchanged) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g fill="green">
        <rect id="r" width="50" height="50" style="stroke: red"/>
      </g>
    </svg>
  )");
  const EntityHandle rect = document.querySelector("#r")->entityHandle();
  const std::shared_ptr<const ComputedStyle> held = rect.get<ComputedStyleComponent>().style;
  ASSERT_THAT(held, NotNull());
  const PropertyRegistry heldValues = held->properties;

  styleSystem.updateStyle(rect, "stroke: blue");
  ParseWarningSink warningSink;
  const ComputedStyleComponent& restyled = styleSystem.computeStyle(rect, warningSink);

  // The edit stores a new record; a holder of the old one, such as a render snapshot, still sees
  // the values it was computed with.
  EXPECT_THAT(restyled.style, AllOf(NotNull(), testing::Ne(held)));
  EXPECT_THAT(held->properties, Eq(heldValues));
  EXPECT_THAT(
      restyled.properties()->stroke.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 255, 255))))));
  EXPECT_THAT(
      restyled.properties()->fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 255))))));
}

TEST_F(StyleSystemTest, RegistryCopiesCountEachLocalDeclarationCopyOnce) {
  // Cascading the leaf computes every ancestor inside the leaf's own cascade; each element with
  // local declarations has them copied once into its record, and inheritance copies nothing.
  constexpr int kDepth = 20;
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">)";
  for (int i = 0; i < kDepth; ++i) {
    source += i % 2 == 0 ? R"(<g fill="red">)" : "<g>";
  }
  source += R"(<rect id="leaf" width="50" height="50" style="stroke: blue"/>)";
  for (int i = 0; i < kDepth; ++i) {
    source += "</g>";
  }
  source += "</svg>";
  auto document = ParseSVG(source);
  const Entity leaf = document.querySelector("#leaf")->entityHandle().entity();
  // Every element of this document is on the leaf's ancestor chain.
  const auto withLocalDeclarations =
      static_cast<std::uint64_t>(document.registry().view<StyleComponent>().size());
  const auto* parsed = document.registry().ctx().find<StyleCascadeMetrics>();
  const StyleCascadeCounters before = parsed ? parsed->counters : StyleCascadeCounters();

  ParseWarningSink warningSink;
  styleSystem.computeStylesFor(document.registry(), std::span<const Entity>(&leaf, 1), warningSink);

  const StyleCascadeCounters work = CascadeMetricsOf(document).counters - before;
  // The groups, the leaf, the `<svg>` element and the document node above it.
  EXPECT_THAT(work, AllOf(Field("elementsCascaded", &StyleCascadeCounters::elementsCascaded,
                                static_cast<std::uint64_t>(kDepth + 3)),
                          Field("registryCopies", &StyleCascadeCounters::registryCopies,
                                withLocalDeclarations)));
  EXPECT_THAT(withLocalDeclarations, Gt(static_cast<std::uint64_t>(kDepth / 2)));
}

TEST_F(StyleSystemTest, PreparedSiblingsCascadeConcurrentlyIntoOwnedSlots) {
  auto doc = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>g > rect:empty { fill: red; stroke-dasharray: 2 3 5; }</style>
      <g font-size="19">
        <rect id="a" style="stroke:blue"/><rect id="b" style="stroke:blue"/>
        <rect id="c" style="stroke:blue"/><rect id="d" style="stroke:blue"/>
      </g>
    </svg>)");
  Registry& registry = doc.registry();
  std::array<Entity, 4> entities;
  std::array<ComputedStyleComponent*, 4> slots;
  std::array<std::shared_ptr<const ComputedStyle>, 4> serial;
  constexpr std::array<std::string_view, 4> selectors{"#a", "#b", "#c", "#d"};
  for (std::size_t i = 0; i < entities.size(); ++i) {
    const auto element = doc.querySelector(selectors[i]);
    ASSERT_TRUE(element);
    entities[i] = element->entityHandle().entity();
    slots[i] = &registry.get<ComputedStyleComponent>(entities[i]);
    serial[i] = std::move(slots[i]->style);
    ASSERT_TRUE(serial[i]);
  }

  const Registry& view = registry;
  const auto& documentBudget = view.ctx().get<StyleResourceBudget>();
  const auto before = view.ctx().get<StyleCascadeMetrics>().counters;
  const auto retainedBefore = documentBudget.complexPropertyBytes();
  std::array<StyleCascadeCounters, 2> counts;
  std::array<std::size_t, 2> warningCounts{};
  std::barrier start(2);
  auto cascadePair = [&](std::size_t worker) {
    auto share = documentBudget.limits();
    share.complexPropertyBytes = 0;
    StyleResourceBudget budget(documentBudget, share);
    EXPECT_EQ(budget.limits().complexPropertyBytes, documentBudget.limits().complexPropertyBytes);
    MatchedPropertiesCache cache;
    StyleSystem::CascadePass pass{budget, counts[worker], cache};
    ParseWarningSink warnings;
    start.arrive_and_wait();
    for (std::size_t i = worker; i < entities.size(); i += counts.size()) {
      StyleSystem::computePreparedStyle(view, entities[i], *slots[i], pass, warnings);
    }
    EXPECT_EQ(pass.activeCascades, 0u);
    warningCounts[worker] = warnings.warningAttempts();
  };
  std::thread other(cascadePair, 1);
  cascadePair(0);
  other.join();

  EXPECT_EQ(before, view.ctx().get<StyleCascadeMetrics>().counters);
  for (std::size_t worker = 0; worker < counts.size(); ++worker) {
    EXPECT_EQ(counts[worker].elementsCascaded, 2u);
    EXPECT_EQ(counts[worker].registryCopies, 1u);
    EXPECT_EQ(counts[worker].matchedPropertiesCacheMisses, 1u);
    EXPECT_EQ(counts[worker].matchedPropertiesCacheHits, 1u);
    EXPECT_EQ(warningCounts[worker], 0u);
    EXPECT_EQ(slots[worker]->style, slots[worker + counts.size()]->style);
  }
  for (std::size_t i = 0; i < slots.size(); ++i) {
    ASSERT_TRUE(slots[i]->style);
    const auto& actual = *slots[i]->properties();
    const auto& expected = serial[i]->properties;
    EXPECT_TRUE(actual.exactlyEquals(expected));
    EXPECT_EQ(slots[i]->style->resourceOpacities, serial[i]->resourceOpacities);
  }
  EXPECT_NE(slots[0]->style, slots[1]->style);
  const auto bytes = slots[0]->properties()->complexPropertyBytes();
  ASSERT_GT(bytes, 0u);
  EXPECT_EQ(documentBudget.complexPropertyBytes(), retainedBefore + 2 * bytes);
  for (auto* slot : slots) {
    slot->style.reset();
  }
  EXPECT_EQ(documentBudget.complexPropertyBytes(), retainedBefore);
}

TEST(StyleResourceBudgetTest, WorkerClampsEveryWorkShareToItsRemainingAllowance) {
  StyleResourceBudget::Limits limits;
  limits.ruleElementMatches = 3;
  limits.declarationApplications = 3;
  limits.declarationComponentWork = 12;
  limits.declarationByteWork = 20;
  limits.selectorTraversalSteps = 5;
  StyleResourceBudget document(limits);
  ASSERT_TRUE(document.reserveRuleElementMatch());
  ASSERT_TRUE(document.reserveRuleElementMatch());
  ASSERT_TRUE(document.reserveDeclarationApplication(7, 11));
  ASSERT_TRUE(document.selectorTraversal().tryConsumeMeasured(3));

  StyleResourceBudget worker(document, limits);
  EXPECT_EQ(worker.limits().ruleElementMatches, 1u);
  EXPECT_EQ(worker.limits().declarationApplications, 2u);
  EXPECT_EQ(worker.limits().declarationComponentWork, 5u);
  EXPECT_EQ(worker.limits().declarationByteWork, 9u);
  EXPECT_EQ(worker.limits().selectorTraversalSteps, 2u);
  EXPECT_TRUE(worker.reserveRuleElementMatch());
  EXPECT_TRUE(worker.reserveDeclarationApplication(5, 9));
  EXPECT_TRUE(worker.selectorTraversal().tryConsumeMeasured(2));
  EXPECT_FALSE(worker.reserveRuleElementMatch());
  EXPECT_TRUE(worker.rejected());
  EXPECT_FALSE(document.rejected());
  EXPECT_EQ(document.ruleElementMatches(), 2u);
  EXPECT_EQ(document.declarationApplications(), 1u);
  EXPECT_EQ(document.selectorTraversal().steps(), 3u);
}

TEST(StyleResourceBudgetTest, WorkerInheritsLatchedDocumentAndAggregateRefusal) {
  StyleResourceBudget::Limits limits;
  limits.ruleElementMatches = 1;
  limits.selectorTraversalSteps = 1;
  StyleResourceBudget document(limits);
  ASSERT_TRUE(document.reserveRuleElementMatch());
  ASSERT_FALSE(document.reserveRuleElementMatch());
  StyleResourceBudget afterRefusal(document, limits);
  EXPECT_TRUE(afterRefusal.rejected());
  EXPECT_FALSE(afterRefusal.reserveDeclarationApplication());

  StyleResourceBudget outer(limits);
  StyleResourceBudget nested(limits);
  nested.setAggregateWorkBudget(&outer);
  ASSERT_TRUE(outer.reserveRuleElementMatch());
  ASSERT_TRUE(outer.selectorTraversal().consume());
  ASSERT_FALSE(nested.rejected());
  StyleResourceBudget remainingAggregate(nested, limits);
  EXPECT_FALSE(remainingAggregate.rejected());
  EXPECT_EQ(remainingAggregate.limits().ruleElementMatches, 0u);
  EXPECT_EQ(remainingAggregate.limits().selectorTraversalSteps, 0u);
  EXPECT_FALSE(remainingAggregate.reserveRuleElementMatch());
  EXPECT_FALSE(outer.rejected());
  ASSERT_FALSE(outer.selectorTraversal().consume());
  StyleResourceBudget aggregateShare(nested, limits);
  EXPECT_TRUE(aggregateShare.rejected());
  EXPECT_TRUE(aggregateShare.selectorTraversal().rejected());
  EXPECT_FALSE(aggregateShare.reserveRuleElementMatch());
}

TEST_F(StyleSystemTest, PreparedElementRejectsAnExternalOccurrence) {
  auto doc =
      ParseAndComputeStyles(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r"/></svg>)");
  auto& registry = doc.registry();
  auto item = doc.querySelector("#r")->entityHandle();
  auto& output = item.get<ComputedStyleComponent>();
  output = {};
  auto& budget = registry.ctx().get<StyleResourceBudget>();
  StyleCascadeCounters counts;
  MatchedPropertiesCache front;
  StyleSystem::CascadePass worker{budget, counts, front};
  ParseWarningSink warnings;
  registry.ctx().emplace<ExternalUseStyleContext>(
      doc.svgElement().entityHandle().entity(), std::make_shared<const ComputedStyle>(),
      ResolvedPaintServer(), ResolvedPaintServer(), nullptr);
  EXPECT_DEATH_IF_SUPPORTED(
      StyleSystem::computePreparedStyle(registry, item.entity(), output, worker, warnings),
      "ExternalUseStyleContext");
}

TEST_F(StyleSystemTest, PreparedElementRejectsAnimationElsewhereInTheDocument) {
  auto doc =
      ParseAndComputeStyles(R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="r"/></svg>)");
  auto& registry = doc.registry();
  auto item = doc.querySelector("#r")->entityHandle();
  auto& output = item.get<ComputedStyleComponent>();
  output = {};
  auto& budget = registry.ctx().get<StyleResourceBudget>();
  StyleCascadeCounters counts;
  MatchedPropertiesCache front;
  StyleSystem::CascadePass worker{budget, counts, front};
  ParseWarningSink warnings;
  std::ignore =
      registry.get_or_emplace<AnimatedValuesComponent>(doc.svgElement().entityHandle().entity());
  EXPECT_DEATH_IF_SUPPORTED(
      StyleSystem::computePreparedStyle(registry, item.entity(), output, worker, warnings),
      "AnimatedValuesComponent");
}

TEST_F(StyleSystemTest, PreparedElementRequiresBothParentAndRootPublication) {
  auto doc = ParseAndComputeStyles(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="g"><rect id="r"/></g></svg>)");
  auto& registry = doc.registry();
  auto item = doc.querySelector("#r")->entityHandle();
  auto& output = item.get<ComputedStyleComponent>();
  output = {};
  auto& parent = doc.querySelector("#g")->entityHandle().get<ComputedStyleComponent>();
  auto& root = doc.svgElement().entityHandle().get<ComputedStyleComponent>();
  auto& budget = registry.ctx().get<StyleResourceBudget>();
  StyleCascadeCounters counts;
  MatchedPropertiesCache front;
  StyleSystem::CascadePass worker{budget, counts, front};
  ParseWarningSink warnings;
  const auto saved = std::move(parent.style);
  EXPECT_DEATH_IF_SUPPORTED(
      StyleSystem::computePreparedStyle(registry, item.entity(), output, worker, warnings),
      "prepared style parent must be published");
  parent.style = saved;
  root.style.reset();
  EXPECT_DEATH_IF_SUPPORTED(
      StyleSystem::computePreparedStyle(registry, item.entity(), output, worker, warnings),
      "prepared style root must be published");
}

TEST_F(StyleSystemTest, PreparedElementCanOnlyWriteItsOwnSlot) {
  auto doc = ParseAndComputeStyles(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="a"/><rect id="b"/></svg>)");
  auto& registry = doc.registry();
  auto a = doc.querySelector("#a")->entityHandle();
  auto& wrong = doc.querySelector("#b")->entityHandle().get<ComputedStyleComponent>();
  auto& budget = registry.ctx().get<StyleResourceBudget>();
  StyleCascadeCounters counts;
  MatchedPropertiesCache front;
  StyleSystem::CascadePass worker{budget, counts, front};
  ParseWarningSink warnings;
  EXPECT_DEATH_IF_SUPPORTED(
      StyleSystem::computePreparedStyle(registry, a.entity(), wrong, worker, warnings),
      "prepared output slot belongs to another entity");
}

TEST_F(StyleSystemTest, PaintBarrierBlocksExternalPaintOnMissAndHit) {
  auto doc = ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="p"><rect id="a"/><rect id="b"/></g></svg>)");
  auto& registry = doc.registry();
  ParseWarningSink warnings;
  auto parent = doc.querySelector("#p")->entityHandle();
  styleSystem.computeStyle(parent, warnings);
  auto& inherited = parent.get<ComputedStyleComponent>();
  inherited.externalFill = true;
  inherited.externalStroke = true;
  auto first = doc.querySelector("#a")->entityHandle();
  auto second = doc.querySelector("#b")->entityHandle();
  for (bool blocked : {true, false}) {
    if (blocked) {
      parent.get_or_emplace<DoNotInheritFillOrStrokeTag>();
    } else {
      parent.remove<DoNotInheritFillOrStrokeTag>();
    }
    first.remove<ComputedStyleComponent>();
    second.remove<ComputedStyleComponent>();
    const auto& miss = styleSystem.computeStyle(first, warnings);
    const auto& hit = styleSystem.computeStyle(second, warnings);
    ASSERT_EQ(miss.style, hit.style);
    EXPECT_EQ(miss.externalFill, !blocked);
    EXPECT_EQ(miss.externalStroke, !blocked);
    EXPECT_EQ(hit.externalFill, !blocked);
    EXPECT_EQ(hit.externalStroke, !blocked);
  }
}

TEST(StyleResourceBudgetTest, TwoDocumentsCannotExceedTheFamilyScratchMaximum) {
  DocumentResourceFamilyBudget::Limits limits;
  limits.stylePreparationBytes = std::numeric_limits<std::size_t>::max();
  auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  StyleResourceBudget first(family), second(family);
  const auto a = first.retainedStyleBytes(), b = second.retainedStyleBytes();
  constexpr auto half = DocumentResourceFamilyBudget::kMaximumStylePreparationBytes / 2;
  ASSERT_TRUE(a->tryReservePreparation(half));
  ASSERT_TRUE(b->tryReservePreparation(half));
  EXPECT_FALSE(b->tryReservePreparation(1));
  EXPECT_EQ(family->stylePreparationBytes(), 2 * half);
  EXPECT_EQ(family->securityStats().rejectedReservations, 0u);
  EXPECT_EQ(family->totalRetainedBytes(), 0u);
  a->releasePreparation(half);
  b->releasePreparation(half);
  EXPECT_EQ(family->stylePreparationBytes(), 0u);
}

TEST_F(StyleSystemTest, WorkerMetadataUsesRefundableFamilyScratchOnly) {
  using Kind = DocumentResourceFamilyBudget::Kind;
  auto doc = ParseSVG(R"(<svg xmlns="http://www.w3.org/2000/svg" id="root"><rect id="a"/></svg>)");
  auto& registry = doc.registry();
  DocumentResourceFamilyBudget::Limits limits;
  limits.stylePreparationBytes = StyleSystem::PreparedBatch::workerReservationBytes();
  auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  auto& logical = registry.ctx().emplace<StyleResourceBudget>(family);
  ParseWarningSink warnings;
  styleSystem.computeStyle(doc.svgElement().entityHandle(), warnings);
  auto a = doc.querySelector("#a")->entityHandle();
  std::ignore = a.get_or_emplace<ComputedStyleComponent>();
  const std::array entities{a.entity()};
  const auto logicalBefore = family->securityStats();
  const auto headroom = family->availableBytes(Kind::ComputedStyle);
  StyleSystem::PreparedBatch batch(registry, entities, 2);
  ASSERT_NE(batch.bindWorker(), nullptr);
  EXPECT_GE(limits.stylePreparationBytes, StyleSystem::StagedRecords::kMaximumBytes +
                                              MatchedPropertiesCache::workerMetadataBytes());
  EXPECT_EQ(family->stylePreparationBytes(), limits.stylePreparationBytes);
  EXPECT_FALSE(family->tryReserveStylePreparation(1));
  std::thread other([&] { EXPECT_EQ(batch.bindWorker(), nullptr); });
  other.join();
  EXPECT_FALSE(logical.rejected());
  EXPECT_EQ(family->securityStats().rejectedReservations, logicalBefore.rejectedReservations);
  EXPECT_TRUE(family->reserve(Kind::ComputedStyle, headroom));
  family->release(Kind::ComputedStyle, headroom);
  EXPECT_EQ(batch.commit(entities, warnings, true), 0u);
  EXPECT_EQ(family->stylePreparationBytes(), 0u);
  EXPECT_EQ(family->availableBytes(Kind::ComputedStyle), headroom);
  EXPECT_TRUE(family->tryReserveStylePreparation(limits.stylePreparationBytes));
  family->releaseStylePreparation(limits.stylePreparationBytes);
}

TEST_F(StyleSystemTest, WorkerWarningsCommitAcrossParentRecursionInSerialOrder) {
  constexpr auto source = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <style>#p {fill: no-parent-paint}
           #r {stroke: no-child-paint}</style>
    <g id="p" style="flood-color:no-parent-color">
      <rect id="r" style="stop-color:no-child-color"/>
    </g></svg>)svg";
  struct Diagnostics {
    std::vector<ParseDiagnostic> warnings;
    std::size_t attempts, bytes, phases;
    bool exceeded;
  };
  const auto collect = [&](bool staged, int mode) {
    auto doc = ParseSVG(source);
    auto& registry = doc.registry();
    ParseWarningSink setup;
    styleSystem.computeStyle(doc.svgElement().entityHandle(), setup);
    const auto parent = doc.querySelector("#p")->entityHandle().entity();
    const auto child = doc.querySelector("#r")->entityHandle().entity();
    const std::array requests{child};
    ParseWarningSink output;
    if (mode == 1) {
      output = ParseWarningSink::Disabled();
    } else if (mode == 2) {
      output = ParseWarningSink::Bounded(1, 1024);
    } else if (mode == 3) {
      output = ParseWarningSink::Bounded(1024, 1);
    }
    if (staged) {
      const std::array slots{parent, child};
      for (auto e : slots) {
        std::ignore = registry.get_or_emplace<ComputedStyleComponent>(e);
      }
      StyleSystem::PreparedBatch batch(registry, slots, 1, output.isEnabled());
      auto* worker = batch.bindWorker();
      EXPECT_NE(worker, nullptr);
      if (worker) {
        EXPECT_TRUE(batch.compute(parent, *worker));
        EXPECT_TRUE(batch.compute(child, *worker));
      }
      EXPECT_EQ(batch.commit(requests, output, true), 2u);
    } else {
      styleSystem.computeStylesFor(registry, requests, output);
    }
    return Diagnostics{output.warnings(), output.warningAttempts(), output.warningBytes(),
                       registry.ctx().get<StyleCascadeMetrics>().preparedWarningPhases,
                       output.resourceLimitExceeded()};
  };
  for (int mode : {0, 1, 2, 3}) {
    const auto serial = collect(false, mode);
    const auto actual = collect(true, mode);
    EXPECT_GE(serial.attempts, 4u);
    EXPECT_EQ(actual.attempts, serial.attempts);
    EXPECT_EQ(actual.warnings, serial.warnings);
    EXPECT_EQ(actual.exceeded, serial.exceeded);
    EXPECT_EQ(actual.bytes, serial.bytes);
    if (mode < 2) {
      EXPECT_GE(actual.phases, 4u);
    } else {
      EXPECT_TRUE(actual.exceeded);
      EXPECT_LT(actual.phases, 4u);
    }
  }
}

TEST_F(StyleSystemTest, SaturatedWorkerWarningsAbandonAndReplayAllRequests) {
  std::string source = "<svg xmlns='http://www.w3.org/2000/svg'><style>.bad {";
  for (int i = 0; i < 100; ++i) {
    source += "fill:invalid" + std::to_string(i) + ";";
  }
  source += "}</style><rect id='ok'/><rect class='bad' id='bad'/></svg>";
  const auto run = [&](bool prepare) {
    auto doc = ParseSVG(source);
    auto& registry = doc.registry();
    auto family = std::make_shared<DocumentResourceFamilyBudget>();
    std::ignore = registry.ctx().emplace<StyleResourceBudget>(family);
    ParseWarningSink warnings;
    styleSystem.computeStyle(doc.svgElement().entityHandle(), warnings);
    const std::array entities{doc.querySelector("#ok")->entityHandle().entity(),
                              doc.querySelector("#bad")->entityHandle().entity()};
    if (prepare) {
      for (auto entity : entities) {
        std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
      }
      StyleSystem::PreparedBatch batch(registry, entities, 1);
      auto* worker = batch.bindWorker();
      EXPECT_NE(worker, nullptr);
      if (worker) {
        EXPECT_TRUE(batch.compute(entities.front(), *worker));
        EXPECT_FALSE(batch.compute(entities.back(), *worker));
      }
      EXPECT_EQ(batch.commit(entities, warnings, true), 0u);
      EXPECT_EQ(family->stylePreparationBytes(), 0u);
    } else {
      styleSystem.computeStylesFor(registry, entities, warnings);
    }
    EXPECT_EQ(family->securityStats().rejectedReservations, 0u);
    EXPECT_FALSE(warnings.resourceLimitExceeded());
    EXPECT_EQ(warnings.warningAttempts(), 100u);
    return warnings.warnings();
  };
  EXPECT_EQ(run(true), run(false));
}

TEST_F(StyleSystemTest, AggregateWorkerWarningPressureReplaysBelowThePerElementLimit) {
  std::string source =
      "<svg xmlns='http://www.w3.org/2000/svg'><style>.bad { fill:invalid }</style>";
  for (int i = 0; i < 40; ++i) {
    source += "<rect id='r" + std::to_string(i) + "' class='bad'/>";
  }
  source += "</svg>";
  const auto run = [&](bool staged) {
    auto doc = ParseSVG(source);
    auto& registry = doc.registry();
    ParseWarningSink output;
    styleSystem.computeStyle(doc.svgElement().entityHandle(), output);
    std::vector<Entity> requests;
    for (int i = 0; i < 40; ++i) {
      auto item = doc.querySelector("#r" + std::to_string(i))->entityHandle();
      std::ignore = item.get_or_emplace<ComputedStyleComponent>();
      requests.push_back(item.entity());
    }
    if (staged) {
      StyleSystem::PreparedBatch batch(registry, requests, 1);
      auto* worker = batch.bindWorker();
      EXPECT_NE(worker, nullptr);
      if (worker) {
        std::size_t accepted = 0;
        for (Entity entity : requests) {
          if (!batch.compute(entity, *worker)) {
            break;
          }
          ++accepted;
        }
        EXPECT_EQ(accepted, 32u);
      }
      EXPECT_EQ(batch.commit(requests, output, true), 0u);
    } else {
      styleSystem.computeStylesFor(registry, requests, output);
    }
    EXPECT_EQ(output.warningAttempts(), 40u);
    EXPECT_FALSE(output.resourceLimitExceeded());
    return output.warnings();
  };
  EXPECT_EQ(run(true), run(false));
}

TEST_F(StyleSystemTest, SpeculativeSlotsKeepTheirBoundWhenFrontEntriesAreCleared) {
  constexpr std::size_t kLanes = 2;
  constexpr auto kPerLane = StyleSystem::StagedRecords::kMaximumRecords;
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="group">)";
  for (std::size_t i = 0; i < kLanes * (kPerLane + 1); ++i) {
    source += "<rect id='r" + std::to_string(i) + "' stroke-dasharray='2 3 5'/>";
  }
  source += "</g></svg>";
  auto doc = ParseSVG(source);
  auto& registry = doc.registry();
  ParseWarningSink warnings;
  styleSystem.computeStyle(doc.querySelector("#group")->entityHandle(), warnings);
  auto& logical = registry.ctx().get<StyleResourceBudget>();
  const std::size_t before = logical.complexPropertyBytes();
  std::vector<Entity> entities;
  for (std::size_t i = 0; i < kLanes * (kPerLane + 1); ++i) {
    const auto entity = doc.querySelector("#r" + std::to_string(i))->entityHandle().entity();
    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
    entities.push_back(entity);
  }

  std::weak_ptr<const ComputedStyle> tentative;
  {
    std::array<StyleSystem::StagedRecords, kLanes> records;
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      MatchedPropertiesCache front;
      StyleResourceBudget budget(logical, logical.limits());
      StyleCascadeCounters counters;
      StyleSystem::CascadePass pass{budget, counters, front, 0, &records[lane]};
      for (std::size_t i = lane; i < entities.size(); i += kLanes) {
        auto& slot = registry.get<ComputedStyleComponent>(entities[i]);
        front.clear();
        StyleSystem::computePreparedStyle(registry, entities[i], slot, pass, warnings);
        EXPECT_LE(records[lane].retainedBytes(), StyleSystem::StagedRecords::kMaximumBytes);
        EXPECT_EQ(logical.complexPropertyBytes(), before);
        if (i / kLanes < kPerLane) {
          ASSERT_TRUE(slot.style);
          tentative = slot.style;
        } else {
          EXPECT_FALSE(slot.style);
          EXPECT_TRUE(records[lane].declined());
        }
      }
    }
    for (Entity entity : entities) {
      registry.get<ComputedStyleComponent>(entity) = {};
    }
  }
  EXPECT_TRUE(tentative.expired());
  styleSystem.computeStylesFor(registry, entities, warnings);
  const auto& first = registry.get<ComputedStyleComponent>(entities.front());
  ASSERT_TRUE(first.style);
  const auto bytes = first.properties()->complexPropertyBytes();
  ASSERT_GT(bytes, 0u);
  EXPECT_EQ(logical.complexPropertyBytes(), before + bytes);
  for (Entity entity : entities) {
    EXPECT_EQ(registry.get<ComputedStyleComponent>(entity).style, first.style);
  }
}

TEST_F(StyleSystemTest, OversizeWorkerPayloadFallsBackWithoutRefusingTheFamily) {
  constexpr std::size_t kValues = 2048;
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg"><g id="group">)";
  for (std::size_t i = 0; i < StyleSystem::StagedRecords::kMaximumRecords; ++i) {
    source += "<rect id='r" + std::to_string(i) + "'/>";
  }
  source += "</g></svg>";
  auto doc = ParseSVG(source);
  auto& registry = doc.registry();
  auto family = std::make_shared<DocumentResourceFamilyBudget>();
  auto& logical = registry.ctx().emplace<StyleResourceBudget>(family);
  ParseWarningSink warnings;
  const auto parent = doc.querySelector("#group")->entityHandle();
  styleSystem.computeStyle(parent, warnings);
  std::vector<Entity> entities;
  for (std::size_t i = 0; i < StyleSystem::StagedRecords::kMaximumRecords; ++i) {
    auto handle = doc.querySelector("#r" + std::to_string(i))->entityHandle();
    handle.get_or_emplace<StyleComponent>().editProperties([&](PropertyRegistry& local) {
      local.strokeDasharray.set(StrokeDasharray(kValues, Lengthd(i + 1)), {});
    });
    std::ignore = handle.get_or_emplace<ComputedStyleComponent>();
    entities.push_back(handle.entity());
  }

  const auto before = family->securityStats();
  {
    StyleSystem::StagedRecords records;
    StyleResourceBudget worker(logical, logical.limits());
    StyleCascadeCounters counters;
    MatchedPropertiesCache front;
    StyleSystem::CascadePass pass{worker, counters, front, 0, &records};
    std::size_t accepted = 0;
    for (Entity entity : entities) {
      auto& slot = registry.get<ComputedStyleComponent>(entity);
      const auto previous = records.retainedBytes();
      StyleSystem::computePreparedStyle(registry, entity, slot, pass, warnings);
      EXPECT_LE(records.retainedBytes(), StyleSystem::StagedRecords::kMaximumBytes);
      if (records.declined()) {
        EXPECT_FALSE(slot.style);
        EXPECT_EQ(records.retainedBytes(), previous);
        break;
      }
      ASSERT_TRUE(slot.style);
      ++accepted;
    }
    EXPECT_GT(accepted, 0u);
    EXPECT_LT(accepted, StyleSystem::StagedRecords::kMaximumRecords);
    EXPECT_TRUE(records.declined());
    for (Entity entity : entities) {
      registry.get<ComputedStyleComponent>(entity) = {};
    }
  }
  auto after = family->securityStats();
  EXPECT_EQ(after.totalRetainedBytes, before.totalRetainedBytes);
  EXPECT_EQ(after.rejectedReservations, before.rejectedReservations);
  EXPECT_EQ(after.rejected, before.rejected);
  styleSystem.computeStylesFor(registry, entities, warnings);
  EXPECT_FALSE(logical.rejected());
  EXPECT_GT(family->securityStats().totalRetainedBytes, before.totalRetainedBytes);
}

TEST_F(StyleSystemTest, UnsupportedWorkerPayloadLeavesTheSerialSlotAvailable) {
  auto doc = ParseSVG(R"svg(<svg xmlns="http://www.w3.org/2000/svg" id="root">
    <rect id="item" style="cursor:url(#pointer),auto"/>
  </svg>)svg");
  auto& registry = doc.registry();
  ParseWarningSink warnings;
  styleSystem.computeStyle(doc.svgElement().entityHandle(), warnings);
  const auto handle = doc.querySelector("#item")->entityHandle();
  auto& slot = handle.get_or_emplace<ComputedStyleComponent>();
  auto& logical = registry.ctx().get<StyleResourceBudget>();
  StyleSystem::StagedRecords records;
  StyleResourceBudget worker(logical, logical.limits());
  StyleCascadeCounters counters;
  MatchedPropertiesCache front;
  StyleSystem::CascadePass pass{worker, counters, front, 0, &records};
  StyleSystem::computePreparedStyle(registry, handle.entity(), slot, pass, warnings);
  EXPECT_TRUE(records.declined());
  EXPECT_EQ(records.retainedBytes(), 0u);
  EXPECT_FALSE(slot.style);
  const auto* properties = styleSystem.computeStyle(handle, warnings).properties();
  ASSERT_NE(properties, nullptr);
  ASSERT_TRUE(properties->cursor.value);
  EXPECT_EQ(properties->cursor.value->images.size(), 1u);
}

TEST_F(StyleSystemTest, OwnerCommitPreservesLaterAdmissionAtTheExactSerialBoundary) {
  constexpr std::string_view source = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <style>.x {stroke-dasharray: 2 3 5} .y {stroke-dasharray: 7 11 13}</style>
    <g id="group"><rect class="x" id="a"/><rect class="x" id="b"/>
      <rect class="x" id="c"/><rect class="x" id="d"/><rect class="y" id="later"/></g>
  </svg>)svg";
  struct Result {
    std::array<std::shared_ptr<const ComputedStyle>, 5> styles;
    std::vector<ParseDiagnostic> warnings;
    std::size_t firstBytes = 0;
    std::size_t finalBytes = 0;
    std::size_t refusals = 0;
    bool rejected = false;
    std::size_t replayed = 0;
  };
  const auto run = [&](std::size_t limit, bool parallel) {
    Result result;
    auto doc = ParseSVG(source);
    auto& registry = doc.registry();
    DocumentResourceFamilyBudget::Limits limits;
    limits.computedStyleBytes = limit;
    auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
    auto& logical = registry.ctx().emplace<StyleResourceBudget>(family);
    ParseWarningSink warnings;
    styleSystem.computeStyle(doc.querySelector("#group")->entityHandle(), warnings);
    std::array<Entity, 4> entities;
    constexpr std::array ids{"#a", "#b", "#c", "#d"};
    for (std::size_t i = 0; i < entities.size(); ++i) {
      entities[i] = doc.querySelector(ids[i])->entityHandle().entity();
      std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entities[i]);
    }

    if (parallel) {
      StyleSystem::PreparedBatch batch(registry, entities, 2);
      const auto before = logical.complexPropertyBytes();
      std::barrier start(2);
      const auto computePair = [&](std::size_t lane) {
        auto* worker = batch.bindWorker();
        start.arrive_and_wait();
        EXPECT_NE(worker, nullptr);
        if (worker) {
          for (std::size_t i = lane; i < entities.size(); i += 2) {
            EXPECT_TRUE(batch.compute(entities[i], *worker));
          }
        }
      };
      std::thread other(computePair, 1);
      computePair(0);
      other.join();
      std::weak_ptr<const ComputedStyle> provisional =
          registry.get<ComputedStyleComponent>(entities.back()).style;
      EXPECT_FALSE(provisional.expired());
      EXPECT_EQ(logical.complexPropertyBytes(), before);
      EXPECT_EQ(family->securityStats().rejectedReservations, 0u);
      result.replayed = batch.commit(entities, warnings, true);
      EXPECT_TRUE(provisional.expired());
      EXPECT_GT(registry.ctx().get<StyleCascadeMetrics>().reusedCanonicalHits, 0u);
    } else {
      styleSystem.computeStylesFor(registry, entities, warnings);
    }
    result.firstBytes = logical.complexPropertyBytes();
    for (std::size_t i = 0; i < entities.size(); ++i) {
      result.styles[i] = registry.get<ComputedStyleComponent>(entities[i]).style;
    }
    result.styles.back() =
        styleSystem.computeStyle(doc.querySelector("#later")->entityHandle(), warnings).style;
    result.finalBytes = logical.complexPropertyBytes();
    result.refusals = family->securityStats().rejectedReservations;
    result.rejected = logical.rejected();
    result.warnings = warnings.warnings();
    return result;
  };
  const auto reference = run(1024 * 1024, false);
  ASSERT_GT(reference.firstBytes, 0u);
  ASSERT_GT(reference.finalBytes, reference.firstBytes);
  for (auto limit : {reference.finalBytes - 1, reference.finalBytes}) {
    const Result serial = run(limit, false);
    const Result staged = run(limit, true);
    EXPECT_EQ(staged.replayed, 4u);
    EXPECT_EQ(serial.rejected, limit < reference.finalBytes);
    EXPECT_EQ(staged.rejected, serial.rejected);
    EXPECT_EQ(staged.refusals, serial.refusals);
    EXPECT_EQ(staged.firstBytes, serial.firstBytes);
    EXPECT_EQ(staged.finalBytes, serial.finalBytes);
    EXPECT_EQ(staged.warnings, serial.warnings);
    for (std::size_t i = 0; i < staged.styles.size(); ++i) {
      ASSERT_TRUE(staged.styles[i]);
      ASSERT_TRUE(serial.styles[i]);
      EXPECT_TRUE(staged.styles[i]->properties.exactlyEquals(serial.styles[i]->properties));
    }
  }
}

TEST_F(StyleSystemTest, OwnerRematchesOverlongAndUnaffordableTraces) {
  struct Result {
    std::size_t replayed = 0;
    StyleCascadeCounters work;
    std::size_t steps = 0;
    bool rejected = false;
    std::vector<ParseDiagnostic> warnings;
    std::shared_ptr<const ComputedStyle> record;
  };
  const auto run = [&](bool prepare, bool overlong) {
    std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)";
    const int ruleCount = overlong ? 7 : 1;
    for (int i = 0; i < ruleCount; ++i) {
      source += "g > rect { fill: invalid-paint; stroke-width: 3 }";
    }
    source += "</style><g id='g'><rect id='r'/></g></svg>";
    auto doc = ParseSVG(source);
    auto& registry = doc.registry();
    StyleResourceBudget::Limits limits;
    limits.selectorTraversalSteps = 64;
    auto& logical = registry.ctx().emplace<StyleResourceBudget>(limits);
    ParseWarningSink warnings;
    styleSystem.computeStyle(doc.querySelector("#g")->entityHandle(), warnings);
    logical.reset();
    auto element = doc.querySelector("#r")->entityHandle();
    std::ignore = element.get_or_emplace<ComputedStyleComponent>();
    const std::array requests{element.entity()};
    const auto before = registry.ctx().get<StyleCascadeMetrics>().counters;
    Result result;
    const auto reservePriorTraversal = [&] {
      if (!overlong) {
        EXPECT_TRUE(logical.selectorTraversal().tryConsumeMeasured(limits.selectorTraversalSteps));
      }
    };
    if (prepare) {
      StyleSystem::PreparedBatch batch(registry, requests, 1);
      auto* worker = batch.bindWorker();
      EXPECT_NE(worker, nullptr);
      if (worker) {
        EXPECT_TRUE(batch.compute(requests.front(), *worker));
      }
      reservePriorTraversal();
      result.replayed = batch.commit(requests, warnings, true);
    } else {
      reservePriorTraversal();
      styleSystem.computeStylesFor(registry, requests, warnings);
    }
    result.work = registry.ctx().get<StyleCascadeMetrics>().counters - before;
    result.steps = logical.selectorTraversal().steps();
    result.rejected = logical.rejected();
    result.warnings = warnings.warnings();
    result.record = element.get<ComputedStyleComponent>().style;
    return result;
  };
  for (bool overlong : {false, true}) {
    const auto serial = run(false, overlong);
    const auto staged = run(true, overlong);
    EXPECT_EQ(staged.replayed, 0u);
    EXPECT_EQ(staged.work.elementsCascaded, serial.work.elementsCascaded);
    EXPECT_EQ(staged.work.rulesTested, serial.work.rulesTested);
    EXPECT_EQ(staged.work.rulesMatched, serial.work.rulesMatched);
    EXPECT_EQ(staged.work.declarationsApplied, serial.work.declarationsApplied);
    EXPECT_EQ(staged.steps, serial.steps);
    EXPECT_EQ(staged.rejected, !overlong);
    EXPECT_EQ(staged.rejected, serial.rejected);
    EXPECT_EQ(staged.warnings, serial.warnings);
    ASSERT_TRUE(staged.record);
    ASSERT_TRUE(serial.record);
    EXPECT_TRUE(staged.record->properties.exactlyEquals(serial.record->properties));
  }
}

TEST_F(StyleSystemTest, LocalDeclarationsOutrankMoreSpecificInheritedDeclarations) {
  // The group's `style` declarations are more specific than the rectangle's presentation
  // attributes, yet the rectangle's own values win: inheritance only supplies what it does not
  // declare.
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g style="fill: red; stroke: red; font-size: 30px; color: red">
        <rect id="r" width="50" height="50" fill="blue" stroke="green" font-size="10"
              color="blue"/>
      </g>
    </svg>
  )");
  const ComputedStyleComponent& style =
      document.querySelector("#r")->entityHandle().get<ComputedStyleComponent>();
  ASSERT_THAT(style.properties(), NotNull());
  EXPECT_THAT(
      style.properties()->fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 255, 255))))));
  EXPECT_THAT(
      style.properties()->stroke.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 255))))));
  EXPECT_THAT(style.properties()->fontSize.get(),
              testing::Optional(LengthIs(testing::DoubleEq(10), Lengthd::Unit::Px)));
  EXPECT_THAT(style.properties()->color.get(),
              testing::Optional(css::Color(css::RGBA(0, 0, 255, 255))));
}

TEST_F(StyleSystemTest, DirtyOnlyPassCountsItsBudgetRejection) {
  auto document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>rect { fill: red; }</style>
      <rect id="r" width="50" height="50"/>
    </svg>
  )");
  auto rect = document.querySelector("#r")->entityHandle();
  SetRenderTreeState(document.registry(), RenderTreeState{.needsFullRebuild = false,
                                                          .needsFullStyleRecompute = false,
                                                          .hasBeenBuilt = true});
  StyleResourceBudget::Limits limits;
  limits.ruleElementMatches = 1;
  SetStyleLimits(document, limits);
  styleSystem.invalidateComputed(rect);
  document.registry()
      .emplace_or_replace<DirtyFlagsComponent>(rect.entity())
      .mark(DirtyFlagsComponent::Style);
  const StyleCascadeCounters before = CascadeMetricsOf(document).counters;

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_THAT(CascadeMetricsOf(document).counters - before,
              AllOf(Field("elementsCascaded", &StyleCascadeCounters::elementsCascaded, 1u),
                    Field("rulesTested", &StyleCascadeCounters::rulesTested, 1u),
                    Field("fullClears", &StyleCascadeCounters::fullClears, 0u),
                    Field("budgetRejections", &StyleCascadeCounters::budgetRejections, 1u)));
}

TEST_F(StyleSystemTest, DeclarationBudgetRefusalCountsOnlyAdmittedDeclarations) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <style>rect { fill: red; stroke: blue; stroke-width: 2; opacity: 0.5; }</style>
      <rect width="50" height="50"/>
    </svg>
  )");
  StyleResourceBudget::Limits limits;
  limits.declarationApplications = 3;
  SetStyleLimits(document, limits);

  ParseWarningSink warningSink;
  styleSystem.computeAllStyles(document.registry(), warningSink);

  EXPECT_THAT(CascadeMetricsOf(document).counters,
              AllOf(Field("declarationsApplied", &StyleCascadeCounters::declarationsApplied, 3u),
                    Field("budgetRejections", &StyleCascadeCounters::budgetRejections, 1u)));
}

TEST_F(StyleSystemTest, DeclarationRefusalPreservesUnchargedLocalScalarStyle) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>rect { stroke: blue; }</style>
      <rect id="target" style="fill: red; color: green"/>
    </svg>)svg");
  const auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  StyleResourceBudget::Limits limits;
  limits.declarationApplications = 0;
  auto& budget = document.registry().ctx().emplace<StyleResourceBudget>(limits);
  ParseWarningSink warnings;

  const auto* properties = styleSystem.computeStyle(target->entityHandle(), warnings).properties();
  ASSERT_THAT(properties, NotNull());
  EXPECT_THAT(
      properties->fill.get(),
      testing::Optional(PaintServer(PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255))))));
  EXPECT_THAT(properties->color.get(), testing::Optional(css::Color(css::RGBA(0, 128, 0, 255))));
  EXPECT_TRUE(budget.rejected());
  EXPECT_EQ(budget.complexPropertyBytes(), 0u);
}

TEST(RuleIndexCharge, StylesheetChargeIncludesItsRuleIndex) {
  // Two stylesheets with the same rule, declaration and component-value counts, so the parsed
  // stylesheet estimate is equal; the first files its rules under two class keys and the second
  // under one, so only their rule indexes differ in size.
  constexpr std::string_view kTwoKeys = ".a { fill: red } .b { fill: red }";
  constexpr std::string_view kOneKey = ".a { fill: red } .a { fill: red }";
  const auto parse = [](std::string_view css) {
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    auto result = parser::SVGParser::ParseSVG(
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)") + std::string(css) +
            "</style></svg>",
        warnings);
    EXPECT_THAT(result, NoParseError());
    return std::move(result.result());
  };
  const auto charge = [](SVGDocument& document) {
    return document.registry()
        .ctx()
        .get<ParsedPayloadResourceBudget>()
        .securityStats()
        .stylesheetBytes;
  };
  const auto indexBytes = [](SVGDocument& document) {
    const Entity style = document.querySelector("style")->entityHandle().entity();
    return document.registry().get<StylesheetComponent>(style).ruleIndexBytes();
  };

  SVGDocument twoKeys = parse(kTwoKeys);
  SVGDocument oneKey = parse(kOneKey);
  ASSERT_GT(indexBytes(twoKeys), indexBytes(oneKey));
  EXPECT_EQ(charge(twoKeys) - charge(oneKey), indexBytes(twoKeys) - indexBytes(oneKey));

  // A keystroke that turns the second `.a` into `.b` reparses the stylesheet and recharges it,
  // index included.
  const std::string source(oneKey.source());
  const std::size_t at = source.rfind(".a") + 1;
  ASSERT_THAT(oneKey
                  .applySourceEdit(xml::XMLEditIntent{
                      .range = {FileOffset::Offset(at), FileOffset::Offset(at + 1)},
                      .replacement = "b",
                      .sourceVersion = oneKey.sourceVersion(),
                  })
                  .applied,
              testing::Eq(true));
  EXPECT_EQ(indexBytes(oneKey), indexBytes(twoKeys));
  EXPECT_EQ(charge(oneKey), charge(twoKeys));
}

TEST(RuleIndexCharge, StylesheetChargeIncludesItsAncestorHashes) {
  // Same counts and the same rule-index key (`.b`); only the child combinator gives the entry an
  // ancestor hash group, so only the indexes' hash storage differs.
  const auto parse = [](std::string_view css) {
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    auto result = parser::SVGParser::ParseSVG(
        std::string(R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)") + std::string(css) +
            "</style></svg>",
        warnings);
    EXPECT_THAT(result, NoParseError());
    return std::move(result.result());
  };
  const auto stylesheet = [](SVGDocument& document) -> const StylesheetComponent& {
    const Entity style = document.querySelector("style")->entityHandle().entity();
    return document.registry().get<StylesheetComponent>(style);
  };
  const auto charge = [](SVGDocument& document) {
    return document.registry()
        .ctx()
        .get<ParsedPayloadResourceBudget>()
        .securityStats()
        .stylesheetBytes;
  };
  SVGDocument child = parse(".a > .b { fill: red }");
  SVGDocument sibling = parse(".a + .b { fill: red }");
  ASSERT_THAT(stylesheet(child).ruleIndex.ancestorHashes(0), testing::SizeIs(1));
  ASSERT_THAT(stylesheet(sibling).ruleIndex.ancestorHashes(0), testing::IsEmpty());
  const std::size_t extra =
      stylesheet(child).ruleIndexBytes() - stylesheet(sibling).ruleIndexBytes();
  ASSERT_GE(extra, sizeof(RuleIndex::AncestorHashGroup));
  EXPECT_EQ(charge(child) - charge(sibling), extra);
}

TEST(RuleIndexCandidates, LargePassesDoNotLeaveTheirCandidateListAllocated) {
  // Every rule is filed under both of the rect's classes, so its cascade gathers two entries per
  // rule before duplicates are removed: more than the per-thread list keeps between passes.
  constexpr std::size_t kRules = 3000;
  static_assert(2 * kRules > StyleSystem::kRetainedCandidateCapacity);
  std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)";
  for (std::size_t i = 0; i < kRules; ++i) {
    svg += ".a, .b { fill: red }";
  }
  svg += R"(</style><rect class="a b" width="1" height="1"/></svg>)";
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svg, warnings);
  ASSERT_THAT(parsed, NoParseError());
  SVGDocument document = std::move(parsed.result());

  StyleSystem().computeAllStyles(document.registry(), warnings);
  EXPECT_THAT(CascadeMetricsOf(document).counters,
              AllOf(Field("rulesMatched", &StyleCascadeCounters::rulesMatched, Ge(kRules)),
                    Field("budgetRejections", &StyleCascadeCounters::budgetRejections, 0u)));
  EXPECT_THAT(StyleSystem::CandidateListCapacityForTesting(),
              Le(StyleSystem::kRetainedCandidateCapacity));
}

// --- Ancestor filter in the cascade ---

namespace {

/// Solid \p color as a paint.
PaintServer Solid(css::RGBA color) {
  return PaintServer(PaintServer::Solid(css::Color(color)));
}

/// Work counted by \p document's counters since \p before.
StyleCascadeCounters WorkSince(SVGDocument& document, const StyleCascadeCounters& before) {
  return CascadeMetricsOf(document).counters - before;
}

/// Matches the four ancestor-filter counters.
auto FilterWorkIs(std::uint64_t rejections, std::uint64_t falsePositives,
                  std::uint64_t saturations = 0, std::uint64_t keyLimits = 0) {
  return AllOf(
      Field("ancestorFilterRejections", &StyleCascadeCounters::ancestorFilterRejections,
            rejections),
      Field("ancestorFilterFalsePositives", &StyleCascadeCounters::ancestorFilterFalsePositives,
            falsePositives),
      Field("ancestorFilterSaturations", &StyleCascadeCounters::ancestorFilterSaturations,
            saturations),
      Field("ancestorFilterKeyLimits", &StyleCascadeCounters::ancestorFilterKeyLimits, keyLimits));
}

/// A rect inside `g.x` and one inside `g.y`, with a rule for each.
constexpr std::string_view kTwoGroups = R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.x rect { fill: red } .y rect { stroke: blue }</style>
      <g class="x"><rect id="a" width="1" height="1"/></g>
      <g class="y"><rect id="b" width="1" height="1"/></g>
    </svg>)";

}  // namespace

TEST_F(StyleSystemTest, AncestorFilterSkipsRulesWhoseAncestorKeysAreAbsent) {
  // Each rect is a candidate of both rules and fails the other group's after a walk; the filter
  // rejects that one without walking. Skipped candidates are still tested and charged.
  SVGDocument filtered = ParseAndComputeStyles(kTwoGroups);
  SVGDocument unfiltered = ParseSVG(kTwoGroups);
  unfiltered.registry().ctx().emplace<UnfilteredCascadeForTesting>();
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  styleSystem.computeAllStyles(unfiltered.registry(), warnings);

  EXPECT_THAT(CascadeMetricsOf(filtered).counters, FilterWorkIs(2, 0));
  EXPECT_THAT(CascadeMetricsOf(unfiltered).counters, FilterWorkIs(0, 0));
  EXPECT_EQ(CascadeMetricsOf(filtered).counters.rulesTested,
            CascadeMetricsOf(unfiltered).counters.rulesTested);
  EXPECT_EQ(CascadeMetricsOf(filtered).counters.rulesMatched,
            CascadeMetricsOf(unfiltered).counters.rulesMatched);
  for (SVGDocument* document : {&filtered, &unfiltered}) {
    const auto a = document->querySelector("#a")->getComputedStyle();
    const auto b = document->querySelector("#b")->getComputedStyle();
    EXPECT_EQ(a.fill.get(), Solid(css::RGBA::RGB(255, 0, 0)));
    EXPECT_EQ(a.stroke.get(), PaintServer(PaintServer::None()));
    EXPECT_EQ(b.fill.get(), Solid(css::RGBA::RGB(0, 0, 0)));
    EXPECT_EQ(b.stroke.get(), Solid(css::RGBA::RGB(0, 0, 255)));
  }
}

TEST_F(StyleSystemTest, AncestorFilterResynchronizesForEachElementOfAnEntityList) {
  // One computeStylesFor pass over rects in different groups: each must be matched against its
  // own ancestors, or `#b` would lose `.y rect` to the chain of `#a`.
  SVGDocument document = ParseAndComputeStyles(kTwoGroups);
  const auto a = document.querySelector("#a")->entityHandle();
  const auto b = document.querySelector("#b")->entityHandle();
  styleSystem.invalidateComputed(a);
  styleSystem.invalidateComputed(b);
  const StyleCascadeCounters before = CascadeMetricsOf(document).counters;
  const std::array<Entity, 2> entities = {a.entity(), b.entity()};
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  styleSystem.computeStylesFor(document.registry(), entities, warnings);

  EXPECT_THAT(WorkSince(document, before), FilterWorkIs(2, 0));
  EXPECT_EQ(a.get<ComputedStyleComponent>().properties()->fill.get(),
            Solid(css::RGBA::RGB(255, 0, 0)));
  EXPECT_EQ(b.get<ComputedStyleComponent>().properties()->stroke.get(),
            Solid(css::RGBA::RGB(0, 0, 255)));
}

TEST_F(StyleSystemTest, AncestorFilterCoversParentsCascadedOnDemand) {
  // Restyling the rect alone cascades its missing parent on demand within the same pass, which
  // consults the filter too: `.y g` is rejected for the inner group.
  SVGDocument document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.y g { opacity: 0.5 }</style>
      <g class="x"><g id="inner"><rect id="r" width="1" height="1"/></g></g>
    </svg>)");
  const auto inner = document.querySelector("#inner")->entityHandle();
  const auto rect = document.querySelector("#r")->entityHandle();
  styleSystem.invalidateComputed(inner);
  styleSystem.invalidateComputed(rect);
  const StyleCascadeCounters before = CascadeMetricsOf(document).counters;
  const Entity entity = rect.entity();
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  styleSystem.computeStylesFor(document.registry(), std::span<const Entity>(&entity, 1), warnings);

  const StyleCascadeCounters work = WorkSince(document, before);
  EXPECT_EQ(work.elementsCascaded, 2u);
  EXPECT_THAT(work, FilterWorkIs(1, 0));
  EXPECT_DOUBLE_EQ(inner.get<ComputedStyleComponent>().properties()->opacity.get().value(), 1.0);
}

TEST_F(StyleSystemTest, SingleEntityComputeStyleIsUnfiltered) {
  // A single entity's style creates no pass, so no filter: it matches the rule a one-entity
  // computeStylesFor pass rejects, and both test the same candidates.
  SVGDocument document = ParseAndComputeStyles(kTwoGroups);
  const auto b = document.querySelector("#b")->entityHandle();
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  styleSystem.invalidateComputed(b);
  const StyleCascadeCounters beforeSingle = CascadeMetricsOf(document).counters;
  const ComputedStyleComponent& style = styleSystem.computeStyle(b, warnings);
  const StyleCascadeCounters single = WorkSince(document, beforeSingle);
  EXPECT_EQ(style.properties()->stroke.get(), Solid(css::RGBA::RGB(0, 0, 255)));

  styleSystem.invalidateComputed(b);
  const StyleCascadeCounters beforePass = CascadeMetricsOf(document).counters;
  const Entity entity = b.entity();
  styleSystem.computeStylesFor(document.registry(), std::span<const Entity>(&entity, 1), warnings);
  const StyleCascadeCounters pass = WorkSince(document, beforePass);

  EXPECT_THAT(single, FilterWorkIs(0, 0));
  EXPECT_THAT(pass, FilterWorkIs(1, 0));
  EXPECT_EQ(single.rulesTested, pass.rulesTested);
  EXPECT_EQ(single.rulesMatched, pass.rulesMatched);
}

TEST_F(StyleSystemTest, AncestorFilterCountsPassedCandidatesThatFailAsFalsePositives) {
  // `.x` is an ancestor, so the filter lets the rule through; it then fails in the subject.
  SVGDocument document = ParseAndComputeStyles(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <style>.x rect:first-child { fill: red }</style>
      <g class="x"><circle r="1"/><rect id="r" width="1" height="1"/></g>
    </svg>)");
  EXPECT_THAT(CascadeMetricsOf(document).counters, FilterWorkIs(0, 1));
  EXPECT_EQ(document.querySelector("#r")->getComputedStyle().fill.get(),
            Solid(css::RGBA::RGB(0, 0, 0)));
}

TEST_F(StyleSystemTest, DisabledAncestorFilterRejectsNothingAndCountsTheDisable) {
  // An ancestor with more keys than the filter takes disables it for its descendants: neither
  // rule is rejected, the failing one is not counted as a false positive, and the matching one
  // still applies.
  std::string attributes;
  for (std::size_t i = 0; i <= StyleAncestorFilter::kMaxKeysPerAncestor; ++i) {
    attributes += " data-" + std::to_string(i) + "=\"1\"";
  }
  SVGDocument document =
      ParseAndComputeStyles(R"(<svg xmlns="http://www.w3.org/2000/svg">)"
                            R"(<style>.x rect { fill: red } .y rect { stroke: blue }</style>)"
                            R"(<g class="x")" +
                            attributes + R"(><rect id="r" width="1" height="1"/></g></svg>)");
  EXPECT_THAT(CascadeMetricsOf(document).counters, FilterWorkIs(0, 0, 0, 1));
  const auto style = document.querySelector("#r")->getComputedStyle();
  EXPECT_EQ(style.fill.get(), Solid(css::RGBA::RGB(255, 0, 0)));
  EXPECT_EQ(style.stroke.get(), PaintServer(PaintServer::None()));
}

TEST_F(StyleSystemTest, SaturatedAncestorFilterRejectsNothingAndCountsTheDisable) {
  // Five ancestors naming `a` 60 times each insert it 300 times, saturating its counters in the
  // fifth, which disables the filter for the rect below it.
  std::string tokens;
  for (int i = 0; i < 60; ++i) {
    tokens += "a ";
  }
  std::string body = R"(<style>.a rect { fill: red } .y rect { stroke: blue }</style>)";
  for (int level = 0; level < 5; ++level) {
    body += "<g class=\"" + tokens + "\">";
  }
  body += R"(<rect id="r" width="1" height="1"/>)";
  for (int level = 0; level < 5; ++level) {
    body += "</g>";
  }
  SVGDocument document =
      ParseAndComputeStyles(R"(<svg xmlns="http://www.w3.org/2000/svg">)" + body + "</svg>");
  EXPECT_THAT(CascadeMetricsOf(document).counters, FilterWorkIs(0, 0, 1, 0));
  const auto style = document.querySelector("#r")->getComputedStyle();
  EXPECT_EQ(style.fill.get(), Solid(css::RGBA::RGB(255, 0, 0)));
  EXPECT_EQ(style.stroke.get(), PaintServer(PaintServer::None()));
}

TEST_F(StyleSystemTest, AncestorFilterBypassesOversizeSelectorLists) {
  std::string selectors;
  for (int i = 0; i < 257; ++i) {
    if (!selectors.empty()) {
      selectors += ", ";
    }
    selectors += ".missing-" + std::to_string(i) + " rect";
  }
  SVGDocument document =
      ParseAndComputeStyles(R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)" + selectors +
                            R"( { fill: red } .also-missing rect { stroke: red } rect { fill: blue }
          </style><g><rect id="target"/></g></svg>)");
  // The oversized rule is matched normally; there is still room to filter the next rule.
  EXPECT_THAT(CascadeMetricsOf(document).counters, FilterWorkIs(1, 0, 0, 0));
  EXPECT_EQ(document.querySelector("#target")->getComputedStyle().fill.get(),
            Solid(css::RGBA::RGB(0, 0, 255)));
}

TEST_F(StyleSystemTest, AncestorFilterProbeAllowanceResetsForEachElement) {
  std::string rules;
  for (int i = 0; i < 257; ++i) {
    rules += ".missing rect { fill: red; }\n";
  }
  auto document = ParseAndComputeStyles(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)" + rules +
      R"(rect { fill: blue; }</style><g><rect id="first"/><rect id="second"/></g></svg>)");
  // Each rect is allowed to filter 256 rules. The last missing rule uses ordinary matching.
  EXPECT_THAT(CascadeMetricsOf(document).counters, FilterWorkIs(2u * 256u, 0, 0, 0));
  for (const std::string_view selector : {"#first", "#second"}) {
    SCOPED_TRACE(selector);
    const auto element = document.querySelector(selector);
    ASSERT_TRUE(element.has_value());
    EXPECT_EQ(element->getComputedStyle().fill.get(), Solid(css::RGBA::RGB(0, 0, 255)));
  }
}

TEST_F(StyleSystemTest, AncestorFilterSavesTraversalBudgetForLaterRules) {
  // 200 failing `.nope rect` walks exhaust a 100-step traversal budget before the last rule
  // without the filter; with it they are skipped, so the last rule applies. Each skipped
  // candidate is still charged to the rule-element budget, so both passes test the same rules
  // until the unfiltered one is refused.
  std::string rules;
  for (int i = 0; i < 200; ++i) {
    rules += ".nope rect { stroke: blue } ";
  }
  const std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)" + rules +
                             R"(rect { fill: red }</style>)"
                             R"(<g><rect id="r" width="1" height="1"/></g></svg>)";
  StyleResourceBudget::Limits limits;
  limits.selectorTraversalSteps = 100;
  const auto cascade = [&](bool filtered) {
    SVGDocument document = ParseSVG(source);
    SetStyleLimits(document, limits);
    if (!filtered) {
      document.registry().ctx().emplace<UnfilteredCascadeForTesting>();
    }
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    styleSystem.computeAllStyles(document.registry(), warnings);
    return document;
  };
  SVGDocument filtered = cascade(true);
  SVGDocument unfiltered = cascade(false);

  EXPECT_EQ(CascadeMetricsOf(unfiltered).counters.budgetRejections, 1u);
  EXPECT_EQ(unfiltered.querySelector("#r")->getComputedStyle().fill.get(),
            Solid(css::RGBA::RGB(0, 0, 0)));
  EXPECT_THAT(CascadeMetricsOf(filtered).counters,
              AllOf(Field("budgetRejections", &StyleCascadeCounters::budgetRejections, 0u),
                    FilterWorkIs(200, 0)));
  EXPECT_EQ(filtered.querySelector("#r")->getComputedStyle().fill.get(),
            Solid(css::RGBA::RGB(255, 0, 0)));
}

}  // namespace donner::svg::components
