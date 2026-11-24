/// @file
/// Differential fuzzer for the rule index: a stylesheet from the input and a small element tree
/// built from it must cascade identically whether each element tests only the index's candidate
/// rules or every rule.
///
/// Input: stylesheet text, a NUL byte, then the tree, three bytes per element. The first byte picks
/// the tag and how far to step out of the open elements before it; the second picks its id and
/// class; the third picks up to two attributes.
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
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ClassComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
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
/// Bytes of input per element.
constexpr std::size_t kElementBytes = 3;
constexpr std::size_t kMaximumElements = 64;

void Require(bool condition) {
  if (!condition) {
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
    svg += ">";
    open.push_back(tag);
  }
  while (!open.empty()) {
    svg += "</" + std::string(open.back()) + ">";
    open.pop_back();
  }
  return svg + "</svg>";
}

std::optional<SVGDocument> Cascade(const std::string& svg, bool unindexed) {
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
  components::StyleSystem().computeAllStyles(document.registry(), warnings);
  return document;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT
  const std::size_t split = input.find('\0');
  const std::string_view css = input.substr(0, std::min<std::size_t>(split, 4096));
  const std::string_view tree =
      split == std::string_view::npos ? std::string_view() : input.substr(split + 1);
  const std::string svg = MakeDocument(css, tree);

  std::optional<SVGDocument> indexed = Cascade(svg, false);
  std::optional<SVGDocument> unindexed = Cascade(svg, true);
  Require(indexed.has_value() == unindexed.has_value());
  if (!indexed) {
    return 0;
  }

  Registry& indexedRegistry = indexed->registry();
  Registry& unindexedRegistry = unindexed->registry();
  const auto& indexedCounters =
      indexedRegistry.ctx().get<components::StyleCascadeMetrics>().counters;
  const auto& unindexedCounters =
      unindexedRegistry.ctx().get<components::StyleCascadeMetrics>().counters;
  Require(indexedCounters.rulesTested <= unindexedCounters.rulesTested);
  if (unindexedCounters.budgetRejections > 0) {
    // Testing every rule ran out of budget; the index may style further, never less.
    Require(indexedCounters.rulesMatched >= unindexedCounters.rulesMatched);
    return 0;
  }
  Require(indexedCounters.rulesMatched == unindexedCounters.rulesMatched);
  Require(indexedCounters.declarationsApplied == unindexedCounters.declarationsApplied);

  // Both documents come from one source, so their entities are created in the same order.
  const auto entities = [](Registry& registry) {
    std::vector<Entity> result;
    for (const Entity entity : registry.view<donner::components::TreeComponent>()) {
      result.push_back(entity);
    }
    return result;
  };
  const std::vector<Entity> indexedEntities = entities(indexedRegistry);
  const std::vector<Entity> unindexedEntities = entities(unindexedRegistry);
  Require(indexedEntities.size() == unindexedEntities.size());
  const components::StyleSystem styleSystem;
  for (std::size_t i = 0; i < indexedEntities.size(); ++i) {
    const auto* indexedStyle =
        indexedRegistry.try_get<components::ComputedStyleComponent>(indexedEntities[i]);
    const auto* unindexedStyle =
        unindexedRegistry.try_get<components::ComputedStyleComponent>(unindexedEntities[i]);
    Require((indexedStyle == nullptr) == (unindexedStyle == nullptr));
    if (indexedStyle != nullptr) {
      // Compare the computed values; the records of two documents are never the same object.
      const PropertyRegistry* indexedProperties = indexedStyle->properties();
      const PropertyRegistry* unindexedProperties = unindexedStyle->properties();
      Require((indexedProperties == nullptr) == (unindexedProperties == nullptr));
      Require(indexedProperties == nullptr || *indexedProperties == *unindexedProperties);
    }
    const auto indexedRules =
        styleSystem.collectMatchedStyleRules(EntityHandle(indexedRegistry, indexedEntities[i]));
    const auto unindexedRules =
        styleSystem.collectMatchedStyleRules(EntityHandle(unindexedRegistry, unindexedEntities[i]));
    Require(indexedRules.size() == unindexedRules.size());
    for (std::size_t rule = 0; rule < indexedRules.size(); ++rule) {
      Require(indexedRules[rule].ruleIndex == unindexedRules[rule].ruleIndex);
      Require(indexedRules[rule].selectorEntryIndex == unindexedRules[rule].selectorEntryIndex);
      Require(indexedRules[rule].specificity == unindexedRules[rule].specificity);
    }
  }
  return 0;
}

}  // namespace donner::svg
