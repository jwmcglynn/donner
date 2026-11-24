/// @file
/// Differential fuzzer for the rule index: a stylesheet from the input and a small element tree
/// built from it must cascade identically whether each element tests only the index's candidate
/// rules or every rule.
///
/// Input: stylesheet text, a NUL byte, then the tree, three bytes per element. The first byte picks
/// the tag and how far to step out of the open elements before it; the second picks its id and
/// class; the third picks up to two attributes and local declarations.
///
/// The vocabulary covers how the index reads keys: mixed-case and namespaced tags, unknown and
/// foreign elements, class lists with repeated tokens and every whitespace separator, and
/// attribute names that differ only in case. Shadow trees and imported stylesheets are left out:
/// both need the document prepared for rendering, which loads resources and instantiates `<use>`
/// targets, and the deterministic differential tests cover them instead.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/MatchedPropertiesCache.h"
#include "donner/svg/components/style/StyleCascadeMetrics.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {
namespace {

constexpr std::array<std::string_view, 16> kTags = {
    "g",    "rect",           "circle",   "Rect",          "path",     "text",   "svg",  "use",
    "RECT", "linearGradient", "clipPath", "foreignObject", "svg:rect", "x:rect", "stop", "unknown"};
constexpr std::array<std::string_view, 4> kIds = {"", "a", "b", "A"};
/// Class lists. `|` stands for a form feed, which XML text cannot hold; it is written through the
/// class component after parsing.
constexpr std::array<std::string_view, 16> kClasses = {
    "",     "a",          "b",     "a b",        "A",   "c a",
    "b  c", "x",          "a a a", "&#9;a&#10;", "a|b", "&#13;b&#13;&#10; a",
    " a ",  "a&#9;&#9;a", "A a",   "b&#9;b b"};
constexpr std::array<std::string_view, 8> kAttributes = {"",
                                                         " fill=\"red\"",
                                                         " data-k=\"1\"",
                                                         " viewBox=\"0 0 1 1\"",
                                                         " VIEWBOX=\"1\"",
                                                         " Fill=\"red\"",
                                                         " data-K=\"1\"",
                                                         " viewbox=\"1\""};
constexpr std::array<std::string_view, 4> kLocalDeclarations = {
    "", " style=\"fill: red\"", " style=\"font-size: 2em; text-decoration-thickness: 1rem\"",
    " style=\"stop-opacity: .25; flood-opacity: .75\""};
/// Bytes of input per element.
constexpr std::size_t kElementBytes = 3;
constexpr std::size_t kMaximumElements = 64;

void Require(bool condition, std::source_location where = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr, "Cascade differential mismatch at line %u\n", where.line());
    std::abort();
  }
}

std::string MakeDocument(std::string_view css, std::string_view tree) {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:svg="http://www.w3.org/2000/svg" )"
      R"(xmlns:x="urn:example:x"><style><![CDATA[)";
  for (std::size_t i = 0; i < css.size(); ++i) {
    // Keep the CDATA section closed only by its own terminator.
    if (css.substr(i, 3) != "]]>") {
      svg.push_back(css[i]);
    }
  }
  svg += "]]></style>";
  std::vector<std::string_view> open;
  for (std::size_t i = 0; i + kElementBytes <= tree.size() && i / kElementBytes < kMaximumElements;
       i += kElementBytes) {
    const auto shape = static_cast<std::uint8_t>(tree[i]);
    const auto keys = static_cast<std::uint8_t>(tree[i + 1]);
    const auto attributes = static_cast<std::uint8_t>(tree[i + 2]);
    for (std::size_t close = shape >> 6; close > 0 && !open.empty(); --close) {
      svg += "</" + std::string(open.back()) + ">";
      open.pop_back();
    }
    const std::string_view tag = kTags[shape & 15];
    svg += "<" + std::string(tag);
    if (const std::string_view id = kIds[keys & 3]; !id.empty()) {
      svg += " id=\"" + std::string(id) + "\"";
    }
    if (const std::string_view className = kClasses[(keys >> 2) & 15]; !className.empty()) {
      svg += " class=\"" + std::string(className) + "\"";
    }
    // A repeated attribute makes the document fail to parse in both cascades.
    svg += kAttributes[attributes & 7];
    svg += kAttributes[(attributes >> 3) & 7];
    svg += kLocalDeclarations[attributes >> 6];
    svg += ">";
    open.push_back(tag);
  }
  while (!open.empty()) {
    svg += "</" + std::string(open.back()) + ">";
    open.pop_back();
  }
  return svg + "</svg>";
}

std::optional<SVGDocument> Cascade(const std::string& svg, bool unindexed, bool cacheDisabled) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(svg, warnings);
  if (parsed.hasError()) {
    return std::nullopt;
  }
  SVGDocument document = std::move(parsed.result());
  // Before the first cascade, so nothing computed from the old class list needs invalidating.
  for (auto [entity, component] : document.registry().view<components::ClassComponent>().each()) {
    std::string className(component.className);
    std::replace(className.begin(), className.end(), '|', '\f');
    component.className = RcString(className);
  }
  if (unindexed) {
    document.registry().ctx().emplace<components::UnindexedCascadeForTesting>();
  }
  if (cacheDisabled) {
    document.registry().ctx().emplace<components::DisableMatchedPropertiesCacheForTesting>();
  }
  components::StyleSystem().computeAllStyles(document.registry(), warnings);
  return document;
}

void RequireSameStylesAndRules(SVGDocument& left, SVGDocument& right) {
  Registry& leftRegistry = left.registry();
  Registry& rightRegistry = right.registry();
  const auto entities = [](Registry& registry) {
    std::vector<Entity> result;
    for (const Entity entity : registry.view<donner::components::TreeComponent>()) {
      result.push_back(entity);
    }
    return result;
  };
  const std::vector<Entity> leftEntities = entities(leftRegistry);
  const std::vector<Entity> rightEntities = entities(rightRegistry);
  Require(leftEntities.size() == rightEntities.size());
  const components::StyleSystem styleSystem;
  for (std::size_t i = 0; i < leftEntities.size(); ++i) {
    const auto* leftStyle =
        leftRegistry.try_get<components::ComputedStyleComponent>(leftEntities[i]);
    const auto* rightStyle =
        rightRegistry.try_get<components::ComputedStyleComponent>(rightEntities[i]);
    Require((leftStyle == nullptr) == (rightStyle == nullptr));
    if (leftStyle != nullptr) {
      const PropertyRegistry* leftProperties = leftStyle->properties();
      const PropertyRegistry* rightProperties = rightStyle->properties();
      Require((leftProperties == nullptr) == (rightProperties == nullptr));
      Require(leftProperties == nullptr || leftProperties->exactlyEquals(*rightProperties));
      Require((leftStyle->resourceOpacities() == nullptr) ==
              (rightStyle->resourceOpacities() == nullptr));
      Require(leftStyle->resourceOpacities() == nullptr ||
              *leftStyle->resourceOpacities() == *rightStyle->resourceOpacities());
      Require(leftStyle->externalFill == rightStyle->externalFill);
      Require(leftStyle->externalStroke == rightStyle->externalStroke);
    }
    const auto leftRules =
        styleSystem.collectMatchedStyleRules(EntityHandle(leftRegistry, leftEntities[i]));
    const auto rightRules =
        styleSystem.collectMatchedStyleRules(EntityHandle(rightRegistry, rightEntities[i]));
    Require(leftRules.size() == rightRules.size());
    for (std::size_t rule = 0; rule < leftRules.size(); ++rule) {
      Require(leftRules[rule].ruleIndex == rightRules[rule].ruleIndex);
      Require(leftRules[rule].selectorEntryIndex == rightRules[rule].selectorEntryIndex);
      Require(leftRules[rule].specificity == rightRules[rule].specificity);
    }
  }
}

void RequireSameLogicalCounters(const components::StyleCascadeCounters& left,
                                const components::StyleCascadeCounters& right) {
  Require(left.elementsCascaded == right.elementsCascaded);
  Require(left.rulesTested == right.rulesTested);
  Require(left.rulesMatched == right.rulesMatched);
  Require(left.declarationsApplied == right.declarationsApplied);
  Require(left.fullClears == right.fullClears);
  Require(left.budgetRejections == right.budgetRejections);
}

void FullRestyle(SVGDocument& document) {
  components::StyleSystem::discardComputedStylesForFullRestyle(document.registry());
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  components::StyleSystem().computeAllStyles(document.registry(), warnings);
}

void RestyleLastEntity(SVGDocument& document) {
  Registry& registry = document.registry();
  std::vector<Entity> entities;
  for (const Entity entity : registry.view<donner::components::TreeComponent>()) {
    entities.push_back(entity);
  }
  if (entities.empty()) {
    return;
  }
  const Entity entity = entities.back();
  components::StyleSystem styleSystem;
  styleSystem.invalidateComputed(EntityHandle(registry, entity));
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  styleSystem.computeStylesFor(registry, std::span<const Entity>(&entity, 1), warnings);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT
  const std::size_t split = input.find('\0');
  const std::string_view css = input.substr(0, std::min<std::size_t>(split, 4096));
  const std::string_view tree =
      split == std::string_view::npos ? std::string_view() : input.substr(split + 1);
  const std::string svg = MakeDocument(css, tree);

  std::optional<SVGDocument> cached = Cascade(svg, false, false);
  std::optional<SVGDocument> uncached = Cascade(svg, false, true);
  std::optional<SVGDocument> unindexed = Cascade(svg, true, true);
  Require(cached.has_value() == uncached.has_value());
  Require(cached.has_value() == unindexed.has_value());
  if (!cached) {
    return 0;
  }

  Registry& cachedRegistry = cached->registry();
  Registry& uncachedRegistry = uncached->registry();
  Registry& unindexedRegistry = unindexed->registry();
  const auto& cachedCounters = cachedRegistry.ctx().get<components::StyleCascadeMetrics>().counters;
  const auto& uncachedCounters =
      uncachedRegistry.ctx().get<components::StyleCascadeMetrics>().counters;
  const auto& unindexedCounters =
      unindexedRegistry.ctx().get<components::StyleCascadeMetrics>().counters;
  Require(cachedCounters.elementsCascaded == uncachedCounters.elementsCascaded);
  Require(cachedCounters.rulesTested == uncachedCounters.rulesTested);
  Require(cachedCounters.rulesMatched == uncachedCounters.rulesMatched);
  Require(cachedCounters.declarationsApplied == uncachedCounters.declarationsApplied);
  Require(cachedCounters.fullClears == uncachedCounters.fullClears);
  Require(cachedCounters.budgetRejections == uncachedCounters.budgetRejections);
  Require(uncachedCounters.matchedPropertiesCacheHits == 0);
  Require(uncachedCounters.declarationParsesSkipped == 0);
  Require(uncachedCounters.rulesTested <= unindexedCounters.rulesTested);
  // Compare the two first frames before either registry advances to the next pass.
  if (unindexedCounters.budgetRejections > 0) {
    Require(uncachedCounters.rulesMatched >= unindexedCounters.rulesMatched);
  } else {
    Require(uncachedCounters.rulesMatched == unindexedCounters.rulesMatched);
    Require(uncachedCounters.declarationsApplied == unindexedCounters.declarationsApplied);
    RequireSameStylesAndRules(*uncached, *unindexed);
  }
  RequireSameStylesAndRules(*cached, *uncached);
  const components::StyleCascadeCounters cachedBeforeFull = cachedCounters;
  const components::StyleCascadeCounters uncachedBeforeFull = uncachedCounters;
  FullRestyle(*cached);
  FullRestyle(*uncached);
  RequireSameStylesAndRules(*cached, *uncached);
  RequireSameLogicalCounters(
      cachedRegistry.ctx().get<components::StyleCascadeMetrics>().counters - cachedBeforeFull,
      uncachedRegistry.ctx().get<components::StyleCascadeMetrics>().counters - uncachedBeforeFull);

  const components::StyleCascadeCounters cachedBeforePartial =
      cachedRegistry.ctx().get<components::StyleCascadeMetrics>().counters;
  const components::StyleCascadeCounters uncachedBeforePartial =
      uncachedRegistry.ctx().get<components::StyleCascadeMetrics>().counters;
  RestyleLastEntity(*cached);
  RestyleLastEntity(*uncached);
  RequireSameStylesAndRules(*cached, *uncached);
  RequireSameLogicalCounters(
      cachedRegistry.ctx().get<components::StyleCascadeMetrics>().counters - cachedBeforePartial,
      uncachedRegistry.ctx().get<components::StyleCascadeMetrics>().counters -
          uncachedBeforePartial);
  return 0;
}

}  // namespace donner::svg
