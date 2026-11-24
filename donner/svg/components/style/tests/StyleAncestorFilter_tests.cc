/// @file
/// \ref StyleAncestorFilter holds exactly the keys of an element's ancestors as selector matching
/// walks them, whatever it held before, and disables itself below an ancestor that saturates a
/// counter or has too many keys.

#include "donner/svg/components/style/StyleAncestorFilter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg::components {
namespace {

using Kind = RuleIndexKey::Kind;

SVGDocument Parse(std::string_view body) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(
      std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" id="root" class="top">)") +
          std::string(body) + "</svg>",
      warnings);
  EXPECT_THAT(parsed, NoParseError());
  return std::move(parsed.result());
}

/// The element with id \p id; `root` is the document's root `<svg>`, which `querySelector` does
/// not return for its own id.
Entity Find(SVGDocument& document, std::string_view id) {
  if (id == "root") {
    return document.svgElement().entityHandle().entity();
  }
  std::optional<SVGElement> element = document.querySelector("#" + std::string(id));
  EXPECT_TRUE(element.has_value()) << id;
  return element ? element->entityHandle().entity() : entt::null;
}

bool Has(const StyleAncestorFilter& filter, Kind kind, std::string_view name) {
  return filter.mayContain(RuleIndex::AncestorKeyHash(kind, name));
}

/// Whether every one of 4,096 spread hash values may be contained: a disabled filter rejects
/// nothing, so a cascade checking it tests every candidate exactly as without the filter.
bool RejectsNothing(const StyleAncestorFilter& filter) {
  for (std::uint32_t i = 0; i < 4096; ++i) {
    if (!filter.mayContain(BloomHash(i * 2654435761u))) {
      return false;
    }
  }
  return true;
}

TEST(StyleAncestorFilter, HoldsEveryAncestorsKeysAndNotTheElementsOwn) {
  SVGDocument document = Parse(R"(<g id="outer" class="a b" data-K="1"><g class="c">)"
                               R"(<rect id="target" class="own" data-own="1"/></g></g>)"
                               R"(<g class="other"><circle id="elsewhere"/></g>)");
  StyleAncestorFilter filter(document.registry());
  filter.synchronizeTo(Find(document, "target"));

  EXPECT_EQ(filter.depth(), 3u);
  EXPECT_TRUE(filter.enabled());
  for (std::string_view name : {"top", "a", "b", "c"}) {
    EXPECT_TRUE(Has(filter, Kind::Class, name)) << name;
  }
  EXPECT_TRUE(Has(filter, Kind::Id, "root"));
  EXPECT_TRUE(Has(filter, Kind::Id, "outer"));
  EXPECT_TRUE(Has(filter, Kind::Type, "svg"));
  EXPECT_TRUE(Has(filter, Kind::Type, "g"));
  // Attribute names are lowercased, and `id` and `class` are attributes too.
  EXPECT_TRUE(Has(filter, Kind::Attribute, "data-k"));
  EXPECT_TRUE(Has(filter, Kind::Attribute, "class"));

  EXPECT_FALSE(Has(filter, Kind::Class, "own"));
  EXPECT_FALSE(Has(filter, Kind::Id, "target"));
  EXPECT_FALSE(Has(filter, Kind::Attribute, "data-own"));
  EXPECT_FALSE(Has(filter, Kind::Type, "rect"));
  EXPECT_FALSE(Has(filter, Kind::Class, "other"));
  // Kinds are salted apart: the class `g` is not the tag `g`.
  EXPECT_FALSE(Has(filter, Kind::Class, "g"));
}

TEST(StyleAncestorFilter, ResynchronizingRemovesAncestorsOffTheNewChain) {
  SVGDocument document =
      Parse(R"(<g id="outer" class="a b"><g class="c"><rect id="target"/></g></g>)"
            R"(<g class="other"><circle id="elsewhere"/></g>)");
  StyleAncestorFilter filter(document.registry());
  filter.synchronizeTo(Find(document, "target"));
  filter.synchronizeTo(Find(document, "elsewhere"));

  EXPECT_EQ(filter.depth(), 2u);
  EXPECT_TRUE(Has(filter, Kind::Class, "other"));
  EXPECT_TRUE(Has(filter, Kind::Class, "top"));
  for (std::string_view name : {"a", "b", "c"}) {
    EXPECT_FALSE(Has(filter, Kind::Class, name)) << name;
  }
  EXPECT_FALSE(Has(filter, Kind::Id, "outer"));

  // A parentless element has no ancestors.
  filter.synchronizeTo(Find(document, "root"));
  EXPECT_EQ(filter.depth(), 0u);
  EXPECT_FALSE(Has(filter, Kind::Class, "top"));
}

TEST(StyleAncestorFilter, AnyVisitOrderEqualsAFreshFilter) {
  SVGDocument document =
      Parse(R"(<g id="g1" class="a"><g id="g2" class="b"><rect id="r1" class="c"/>)"
            R"(<g id="g3" class="a c"><circle id="c1"/><rect id="r2" class="d"/></g></g>)"
            R"(<g id="g4" class="e"><path id="p1"/></g></g>)"
            R"(<g id="g5" class="b d"><g id="g6" class="f"><rect id="r3"/></g></g>)");
  const std::vector<std::string_view> order = {"r2", "root", "p1", "g3", "r3", "c1", "g1",
                                               "g6", "r1",   "g4", "g2", "r2", "g5", "p1"};
  const std::vector<std::pair<Kind, std::string_view>> keys = {
      {Kind::Class, "a"},   {Kind::Class, "b"},      {Kind::Class, "c"},        {Kind::Class, "d"},
      {Kind::Class, "e"},   {Kind::Class, "f"},      {Kind::Class, "top"},      {Kind::Id, "g1"},
      {Kind::Id, "g2"},     {Kind::Id, "g3"},        {Kind::Id, "g4"},          {Kind::Id, "g5"},
      {Kind::Id, "g6"},     {Kind::Id, "root"},      {Kind::Type, "g"},         {Kind::Type, "svg"},
      {Kind::Type, "rect"}, {Kind::Attribute, "id"}, {Kind::Attribute, "class"}};

  StyleAncestorFilter shared(document.registry());
  for (std::string_view id : order) {
    const Entity element = Find(document, id);
    shared.synchronizeTo(element);
    StyleAncestorFilter fresh(document.registry());
    fresh.synchronizeTo(element);
    EXPECT_EQ(shared.depth(), fresh.depth()) << id;
    for (const auto& [kind, name] : keys) {
      EXPECT_EQ(Has(shared, kind, name), Has(fresh, kind, name)) << id << " " << name;
    }
  }
}

TEST(StyleAncestorFilter, ShadowCloneChainRunsThroughTheHostAsMatchingWalksIt) {
  SVGDocument document =
      Parse(R"(<defs><g id="tpl" class="inner"><rect id="leaf" class="leafc"/></g></defs>)"
            R"(<g id="hostparent" class="outer"><use id="host" class="u" href="#tpl"/></g>)");
  Registry& registry = document.registry();
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  EntityHandle host(registry, Find(document, "host"));
  auto& shadow = host.get_or_emplace<ComputedShadowTreeComponent>();
  ShadowTreeSystem().populateInstance(host, shadow, ShadowBranchType::Main, Find(document, "tpl"),
                                      RcString("#tpl"), warnings);
  ASSERT_TRUE(shadow.mainBranch.has_value());
  Entity cloneLeaf = entt::null;
  for (const Entity entity : shadow.mainBranch->shadowEntities) {
    if (registry.get<ShadowEntityComponent>(entity).lightEntity == Find(document, "leaf")) {
      cloneLeaf = entity;
    }
  }
  ASSERT_TRUE(cloneLeaf != entt::null);

  StyleAncestorFilter filter(registry);
  filter.synchronizeTo(cloneLeaf);
  // The clone of the `<g>`, which reads its id and classes from the `<g>`, then the host and
  // its ancestors; not the `<defs>` above the cloned `<g>`.
  EXPECT_EQ(filter.depth(), 4u);
  EXPECT_TRUE(Has(filter, Kind::Class, "inner"));
  EXPECT_TRUE(Has(filter, Kind::Id, "tpl"));
  EXPECT_TRUE(Has(filter, Kind::Class, "u"));
  EXPECT_TRUE(Has(filter, Kind::Id, "host"));
  EXPECT_TRUE(Has(filter, Kind::Type, "use"));
  EXPECT_TRUE(Has(filter, Kind::Class, "outer"));
  EXPECT_TRUE(Has(filter, Kind::Class, "top"));
  EXPECT_FALSE(Has(filter, Kind::Type, "defs"));
  EXPECT_FALSE(Has(filter, Kind::Class, "leafc"));
}

TEST(StyleAncestorFilter, IdAndClassKeysKeepTheirCase) {
  SVGDocument document = Parse(R"(<g id="X" class="A"><rect id="t"/></g>)");
  StyleAncestorFilter filter(document.registry());
  filter.synchronizeTo(Find(document, "t"));
  EXPECT_TRUE(Has(filter, Kind::Id, "X"));
  EXPECT_TRUE(Has(filter, Kind::Class, "A"));
  EXPECT_FALSE(Has(filter, Kind::Id, "x"));
  EXPECT_FALSE(Has(filter, Kind::Class, "a"));
}

TEST(StyleAncestorFilter, TagAndAttributeKeysIgnoreCase) {
  SVGDocument document = Parse(R"(<G class="x" DATA-Y="1"><rect id="t"/></G>)");
  StyleAncestorFilter filter(document.registry());
  filter.synchronizeTo(Find(document, "t"));
  EXPECT_TRUE(Has(filter, Kind::Type, "g"));
  EXPECT_TRUE(Has(filter, Kind::Attribute, "data-y"));
  EXPECT_FALSE(Has(filter, Kind::Type, "G"));
}

/// `count` copies of `token`, space-separated.
std::string Repeat(std::string_view token, int count) {
  std::string result;
  for (int i = 0; i < count; ++i) {
    result += std::string(token) + " ";
  }
  return result;
}

TEST(StyleAncestorFilter, SaturationDisablesBelowTheSaturatingAncestorUntilItIsPopped) {
  // Five ancestors with 60 `a` tokens each insert `a` 300 times; its counters saturate in the
  // fifth. The fourth's descendants have seen it 240 times.
  const std::string a60 = Repeat("a", 60);
  std::string body = R"(<g class="b"><rect id="shallow"/></g>)";
  for (int level = 1; level <= 5; ++level) {
    body += "<g class=\"" + a60 + "\">";
    if (level == 4) {
      body += R"(<rect id="mid"/>)";
    }
  }
  body += R"(<rect id="deep"/>)";
  for (int level = 1; level <= 5; ++level) {
    body += "</g>";
  }
  SVGDocument document = Parse(body);
  StyleAncestorFilter filter(document.registry());

  filter.synchronizeTo(Find(document, "deep"));
  EXPECT_FALSE(filter.enabled());
  EXPECT_TRUE(RejectsNothing(filter));

  // Popping the fifth ancestor rebuilds the filter from every remaining one, the outermost
  // included.
  filter.synchronizeTo(Find(document, "mid"));
  EXPECT_TRUE(filter.enabled());
  EXPECT_FALSE(RejectsNothing(filter));
  EXPECT_TRUE(Has(filter, Kind::Type, "svg"));
  EXPECT_TRUE(Has(filter, Kind::Id, "root"));
  EXPECT_TRUE(Has(filter, Kind::Class, "top"));
  EXPECT_TRUE(Has(filter, Kind::Type, "g"));
  EXPECT_TRUE(Has(filter, Kind::Class, "a"));
  EXPECT_TRUE(Has(filter, Kind::Attribute, "class"));
  EXPECT_FALSE(Has(filter, Kind::Id, "absent"));

  // Back out of the saturated chain, nothing is left of `a`.
  filter.synchronizeTo(Find(document, "deep"));
  filter.synchronizeTo(Find(document, "shallow"));
  EXPECT_TRUE(filter.enabled());
  EXPECT_TRUE(Has(filter, Kind::Class, "b"));
  EXPECT_FALSE(Has(filter, Kind::Class, "a"));
}

TEST(StyleAncestorFilter, AnAncestorWithTooManyKeysDisablesTheFilterBelowIt) {
  std::string attributes;
  for (std::size_t i = 0; i <= StyleAncestorFilter::kMaxKeysPerAncestor; ++i) {
    attributes += " data-" + std::to_string(i) + "=\"1\"";
  }
  SVGDocument document = Parse("<g" + attributes + R"(><rect id="under"/></g>)" +
                               R"(<g class="b"><rect id="beside"/></g>)");
  StyleAncestorFilter filter(document.registry());
  filter.synchronizeTo(Find(document, "under"));
  EXPECT_FALSE(filter.enabled());
  EXPECT_TRUE(RejectsNothing(filter));
  filter.synchronizeTo(Find(document, "beside"));
  EXPECT_TRUE(filter.enabled());
  EXPECT_FALSE(Has(filter, Kind::Attribute, "data-0"));
  EXPECT_TRUE(Has(filter, Kind::Class, "b"));
}

}  // namespace
}  // namespace donner::svg::components
