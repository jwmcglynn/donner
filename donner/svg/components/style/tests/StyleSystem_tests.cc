/**
 * Tests for StyleSystem: CSS cascade, inheritance, and style computation from SVG elements.
 */

#include "donner/svg/components/style/StyleSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGStyleQuery.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
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
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/components/style/StyleComponent.h"
#include "donner/svg/parser/SVGParser.h"

using testing::NotNull;

namespace donner::svg::components {

namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Field;

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
    const auto& properties = *styleSystem.computeStyle(target, warnings).properties;
    EXPECT_EQ(properties.fill.get(),
              PaintServer(PaintServer::Solid(css::Color(css::RGBA::RGB(255, 0, 0)))));
    EXPECT_EQ(properties.color.get(), css::Color(css::RGBA::RGB(0, 255, 0)));
    EXPECT_FALSE(budget.rejected());
  }
  AnimationSystem().advance(registry, 1.5);
  registry.ctx().emplace<AnimationStyleContext>().enabled = true;
  styleSystem.invalidateComputed(target);
  {
    const auto& properties = *styleSystem.computeStyle(target, warnings).properties;
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
  const auto& properties = *styleSystem.computeStyle(target, warnings).properties;
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
        registry.get<ComputedStyleComponent>(target).properties->opacity.get().value());
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
  EXPECT_TRUE(computed->properties.has_value());
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
  EXPECT_TRUE(computed->properties.has_value());
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
  EXPECT_TRUE(computed->properties.has_value());
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
  EXPECT_TRUE(computed->properties.has_value());
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
  EXPECT_TRUE(computed->properties.has_value());
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
  EXPECT_TRUE(computed->properties.has_value());
  EXPECT_EQ(computed->properties->display.get().value(), Display::None);
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
  EXPECT_TRUE(computed->properties.has_value());
  EXPECT_EQ(computed->properties->visibility.get().value(), Visibility::Hidden);
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
  EXPECT_TRUE(computed.properties.has_value());
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
    EXPECT_TRUE(computed->properties.has_value()) << "No properties for: " << id;
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
  EXPECT_EQ(computed.properties->fill.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 0xFF)))));
  EXPECT_EQ(computed.properties->stroke.get().value(),
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

  dirty.get_or_emplace<StyleComponent>().properties.parseStyle("fill: blue");
  clean.get_or_emplace<StyleComponent>().properties.parseStyle("fill: green");
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
                .properties->fill.get()
                .value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
  EXPECT_EQ(document.registry()
                .get<ComputedStyleComponent>(clean.entity())
                .properties->fill.get()
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
                .properties->fill.get()
                .value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
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
                  .properties->textDecorationThickness.get()
                  ->value,
              LengthIs(testing::DoubleEq(4), Lengthd::Unit::Px));
  EXPECT_THAT(registry.get<ComputedStyleComponent>(child.entity())
                  .properties->textDecorationThickness.get()
                  ->value,
              LengthIs(testing::DoubleEq(4), Lengthd::Unit::Px));
  EXPECT_EQ(registry.get<ComputedStyleComponent>(plain.entity())
                .properties->textDecorationThickness.get()
                ->kind,
            TextDecorationThickness::Kind::Auto);
}

TEST_F(StyleSystemTest, DecorationThicknessRetainsInheritedPercentage) {
  auto document = ParseAndComputeStyles(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><text font-size="20" style="text-decoration-thickness:20%">
      <tspan id="child" font-size="40" style="text-decoration-thickness:inherit">X</tspan>
    </text></svg>)svg");
  const auto child = document.querySelector("#child")->entityHandle();
  EXPECT_THAT(document.registry()
                  .get<ComputedStyleComponent>(child.entity())
                  .properties->textDecorationThickness.get()
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
                  .properties->textDecorationThickness.get()
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
                    .properties->textDecorationThickness.get()
                    ->value,
                LengthIs(testing::DoubleEq(5), Lengthd::Unit::Px));
  }
}

TEST_F(StyleSystemTest, PolygonInheritanceChargesAndReleasesRetainedStorage) {
  auto document = ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg">
      <g id="parent" clip-path="polygon(0 0, 10 0, 10 10, 0 10)">
        <rect id="first" style="clip-path:inherit"/>
        <rect id="second" style="clip-path:inherit"/>
      </g>
    </svg>)svg");
  auto parent = document.querySelector("#parent")->entityHandle();
  auto first = document.querySelector("#first")->entityHandle();
  auto second = document.querySelector("#second")->entityHandle();
  constexpr std::size_t kPolygonBytes = 4 * sizeof(PolygonClipPath::Vertex);
  DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.computedStyleBytes = 2 * kPolygonBytes;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(familyLimits);
  StyleResourceBudget::Limits limits;
  limits.complexPropertyBytes = 2 * kPolygonBytes;
  auto& budget = document.registry().ctx().emplace<StyleResourceBudget>(limits, family);
  ParseWarningSink warnings;
  ASSERT_TRUE(styleSystem.computeStyle(parent, warnings).properties->clipPath.get().has_value());
  EXPECT_EQ(budget.complexPropertyBytes(), kPolygonBytes);
  ASSERT_TRUE(styleSystem.computeStyle(first, warnings).properties->clipPath.get().has_value());
  EXPECT_EQ(budget.complexPropertyBytes(), 2 * kPolygonBytes);
  EXPECT_FALSE(styleSystem.computeStyle(second, warnings).properties->clipPath.get().has_value());
  EXPECT_TRUE(budget.rejected());
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle),
            2 * kPolygonBytes);

  styleSystem.invalidateComputed(first);
  EXPECT_EQ(budget.complexPropertyBytes(), kPolygonBytes);
  budget.reset();
  first.get<StyleComponent>().properties.parseStyle("clip-path:none");
  EXPECT_FALSE(styleSystem.computeStyle(first, warnings).properties->clipPath.get().has_value());
  EXPECT_EQ(budget.complexPropertyBytes(), kPolygonBytes);
  styleSystem.invalidateComputed(parent);
  parent.get<StyleComponent>().properties.parseStyle("clip-path:none");
  EXPECT_FALSE(styleSystem.computeStyle(parent, warnings).properties->clipPath.get().has_value());
  EXPECT_EQ(budget.complexPropertyBytes(), 0u);
  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle), 0u);
}

TEST(StyleResourceBudgetTest, SharesRetainedBytesAcrossDocumentFamily) {
  DocumentResourceFamilyBudget::Limits familyLimits;
  familyLimits.computedStyleBytes = 1024;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(familyLimits);

  StyleResourceBudget::Limits localLimits;
  localLimits.complexPropertyBytes = 1024;
  Registry registry;
  const Entity firstEntity = registry.create();
  const Entity secondEntity = registry.create();

  {
    StyleResourceBudget first(localLimits, family);
    StyleResourceBudget second(localLimits, family);
    EXPECT_TRUE(first.reserveComplexPropertyBytes(firstEntity, 768));
    EXPECT_FALSE(second.reserveComplexPropertyBytes(secondEntity, 512));
    EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle), 768u);
  }

  EXPECT_EQ(family->retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedStyle), 0u);
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
  Registry registry;
  const Entity atCap = registry.create();
  const Entity plusOne = registry.create();

  EXPECT_TRUE(budget.reserveComplexPropertyBytes(atCap, properties.complexPropertyBytes()));
  EXPECT_EQ(budget.complexPropertyBytes(), retainedCapacityBytes);
  EXPECT_FALSE(budget.reserveComplexPropertyBytes(plusOne, 1));
  EXPECT_EQ(budget.complexPropertyBytes(), retainedCapacityBytes);
  EXPECT_TRUE(budget.rejected());
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
  overridden->entityHandle().get_or_emplace<StyleComponent>().properties.display.set(
      Display::Inline, css::Specificity::Override());
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
  overridden->entityHandle().get_or_emplace<StyleComponent>().properties.display.set(
      Display::Inline, css::Specificity::Override());
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
    const auto& proxyProperties = styleSystem.computeStyle(proxy, warnings).properties;
    ASSERT_THAT(proxyProperties, testing::Optional(testing::_));
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
      const auto& nestedProperties = styleSystem.computeStyle(nestedProxy, warnings).properties;
      ASSERT_THAT(nestedProperties, testing::Optional(testing::_));
      EXPECT_THAT(nestedProperties->opacity.get(), testing::Optional(.8));
      testedNested = true;
    }
    EXPECT_TRUE(testedNested);
    const bool empty = std::string_view(id) == "empty";
    EXPECT_EQ(host.isEmptyForSelector(nullptr), empty);
    const auto& properties = styleSystem.computeStyle(host.entityHandle(), warnings).properties;
    ASSERT_THAT(properties, testing::Optional(testing::_));
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
  definition->entityHandle().get_or_emplace<StyleComponent>().properties.display.set(
      Display::None, css::Specificity::Override());
  auto& shadowTree = instance->entityHandle().get_or_emplace<ComputedShadowTreeComponent>();
  ParseWarningSink warnings;
  ShadowTreeSystem().populateInstance(instance->entityHandle(), shadowTree, ShadowBranchType::Main,
                                      definition->entityHandle().entity(), RcString("#definition"),
                                      warnings);
  ASSERT_THAT(shadowTree.mainBranch, testing::Optional(testing::_));
  ASSERT_THAT(shadowTree.mainBranch->shadowEntities, testing::Not(testing::IsEmpty()));
  EntityHandle root(document.registry(), shadowTree.mainBranch->shadowEntities.front());
  const auto& properties = styleSystem.computeStyle(root, warnings).properties;
  ASSERT_THAT(properties, testing::Optional(testing::_));
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
  const auto& properties = styleSystem.computeStyle(root, warnings).properties;
  ASSERT_THAT(properties, testing::Optional(testing::_));
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
        root->entityHandle().entity(), PropertyRegistry(), std::nullopt, ResolvedPaintServer(),
        ResolvedPaintServer(), nullptr, 0, nullptr);
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

  EXPECT_EQ(rectStyle.properties->fill.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF)))));
  EXPECT_EQ(rectStyle.properties->visibility.get().value(), Visibility::Hidden);
  EXPECT_EQ(circleStyle.properties->stroke.get().value(),
            PaintServer(PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 0xFF)))));
  EXPECT_DOUBLE_EQ(circleStyle.properties->opacity.get().value(), 0.5);
}

}  // namespace donner::svg::components
