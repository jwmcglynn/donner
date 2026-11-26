/// @file
/// Key choice for every selector shape and candidate collection of \ref RuleIndex.

#include "donner/svg/components/style/RuleIndex.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::svg::components {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using Kind = RuleIndexKey::Kind;

css::Stylesheet Parse(std::string_view css) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  return css::parser::StylesheetParser::Parse(css, warnings);
}

/// Keys of every selector-list entry of the single rule in `<selector> {}`.
std::vector<RuleIndexKey> KeysOf(std::string_view selector) {
  const css::Stylesheet sheet = Parse(std::string(selector) + " { fill: red }");
  std::vector<RuleIndexKey> keys;
  for (const css::SelectorRule& rule : sheet.rules()) {
    for (const css::ComplexSelector& entry : rule.selector.entries) {
      keys.push_back(RuleIndex::KeyFor(entry));
    }
  }
  return keys;
}

RuleIndexKey Key(Kind kind, std::string_view name = {}) {
  return RuleIndexKey{.kind = kind, .name = RcString(name)};
}

TEST(RuleIndexKey, PrefersIdThenClassThenAttributeThenType) {
  EXPECT_THAT(KeysOf("rect.a[x]#b"), ElementsAre(Key(Kind::Id, "b")));
  EXPECT_THAT(KeysOf("rect[x].a"), ElementsAre(Key(Kind::Class, "a")));
  EXPECT_THAT(KeysOf("rect[x]"), ElementsAre(Key(Kind::Attribute, "x")));
  EXPECT_THAT(KeysOf("rect"), ElementsAre(Key(Kind::Type, "rect")));
}

TEST(RuleIndexKey, UsesOnlyTheSubjectCompound) {
  EXPECT_THAT(KeysOf("#a .b"), ElementsAre(Key(Kind::Class, "b")));
  EXPECT_THAT(KeysOf(".a > rect"), ElementsAre(Key(Kind::Type, "rect")));
  EXPECT_THAT(KeysOf("#a + *"), ElementsAre(Key(Kind::Universal)));
  EXPECT_THAT(KeysOf(".a ~ [x]"), ElementsAre(Key(Kind::Attribute, "x")));
}

TEST(RuleIndexKey, KeepsIdAndClassCaseAndLowercasesTypeAndAttribute) {
  EXPECT_THAT(KeysOf("#MixedCase"), ElementsAre(Key(Kind::Id, "MixedCase")));
  EXPECT_THAT(KeysOf(".MixedCase"), ElementsAre(Key(Kind::Class, "MixedCase")));
  EXPECT_THAT(KeysOf("RECT"), ElementsAre(Key(Kind::Type, "rect")));
  EXPECT_THAT(KeysOf("[DATA-X]"), ElementsAre(Key(Kind::Attribute, "data-x")));
}

TEST(RuleIndexKey, NamespacesAndUniversalTypes) {
  EXPECT_THAT(KeysOf("*"), ElementsAre(Key(Kind::Universal)));
  EXPECT_THAT(KeysOf("*|*"), ElementsAre(Key(Kind::Universal)));
  // A namespaced type keeps its name as the key; the namespace is left to the full match.
  EXPECT_THAT(KeysOf("*|rect"), ElementsAre(Key(Kind::Type, "rect")));
  // Namespaced and wildcard attribute selectors give no key.
  EXPECT_THAT(KeysOf("[*|x]"), ElementsAre(Key(Kind::Universal)));
  EXPECT_THAT(KeysOf("rect[*|x]"), ElementsAre(Key(Kind::Type, "rect")));
}

TEST(RuleIndexKey, PseudoClassesAndTheirArgumentsGiveNoKey) {
  for (std::string_view selector :
       {":is(.a)", ":where(#a)", ":not(rect)", ":has(.a)", ":first-child", ":nth-child(2)", ":root",
        ":empty", ":nth-child(1 of .a)"}) {
    SCOPED_TRACE(selector);
    EXPECT_THAT(KeysOf(selector), ElementsAre(Key(Kind::Universal)));
  }
  EXPECT_THAT(KeysOf("rect:not(.a)"), ElementsAre(Key(Kind::Type, "rect")));
  EXPECT_THAT(KeysOf(":has(#a).b"), ElementsAre(Key(Kind::Class, "b")));
}

TEST(RuleIndexKey, EachSelectorListEntryHasItsOwnKey) {
  EXPECT_THAT(KeysOf("#a, .b, rect, *"),
              ElementsAre(Key(Kind::Id, "a"), Key(Kind::Class, "b"), Key(Kind::Type, "rect"),
                          Key(Kind::Universal)));
}

/// Candidates of \p index for an element with the given keys.
std::vector<std::uint32_t> Candidates(const RuleIndex& index, const RuleIndexElementKeys& keys) {
  std::vector<std::uint32_t> candidates = {99, 98};  // Replaced, not appended to.
  index.collectCandidates(keys, candidates);
  return candidates;
}

TEST(RuleIndexCandidates, AscendingWithoutRepeatsAcrossBuckets) {
  const css::Stylesheet sheet = Parse(
      ".a { fill: red }"          // 0
      "rect { fill: red }"        // 1
      "* { fill: red }"           // 2
      ".a, .b, #c { fill: red }"  // 3, under three keys
      ".b .a { fill: red }"       // 4
      "[x] { fill: red }"         // 5
      ".z { fill: red }"          // 6, never a candidate here
      ":not(.z) { fill: red }");  // 7, universal
  const RuleIndex index(sheet.rules());
  ASSERT_EQ(index.ruleCount(), 8u);

  RuleIndexElementKeys keys;
  keys.idName = "c";
  for (std::string_view className : {"a", "b", "a"}) {
    keys.classNames.push_back(className);
  }
  keys.tag = "rect";
  keys.attributeLocalNames.push_back("x");
  keys.attributeLocalNames.push_back("y");
  EXPECT_THAT(Candidates(index, keys), ElementsAre(0, 1, 2, 3, 4, 5, 7));
}

TEST(RuleIndexCandidates, UniversalRulesAreEveryElementsCandidates) {
  const css::Stylesheet sheet = Parse("* { fill: red } .a { fill: red } :root { fill: red }");
  const RuleIndex index(sheet.rules());
  EXPECT_THAT(Candidates(index, RuleIndexElementKeys{}), ElementsAre(0, 2));
}

TEST(RuleIndexCandidates, TypeAndAttributeLookupsIgnoreCaseIdAndClassDoNot) {
  const css::Stylesheet sheet =
      Parse("rect { fill: red } [viewBox] { fill: red } #Id { fill: red } .Cls { fill: red }");
  const RuleIndex index(sheet.rules());

  RuleIndexElementKeys keys;
  keys.tag = "RECT";
  keys.attributeLocalNames.push_back("viewbox");
  keys.idName = "id";
  keys.classNames.push_back("cls");
  EXPECT_THAT(Candidates(index, keys), ElementsAre(0, 1));

  keys.idName = "Id";
  keys.classNames.clear();
  keys.classNames.push_back("Cls");
  EXPECT_THAT(Candidates(index, keys), ElementsAre(0, 1, 2, 3));
}

/// Rule positions \p index gathers for \p keys before removing duplicates; the candidates must
/// still be every rule once.
std::size_t Gathered(const RuleIndex& index, const RuleIndexElementKeys& keys,
                     std::size_t expectedCandidates) {
  std::vector<std::uint32_t> candidates;
  const std::size_t gathered = index.collectCandidates(keys, candidates);
  EXPECT_EQ(candidates.size(), expectedCandidates);
  return gathered;
}

TEST(RuleIndexCandidates, RepeatedClassTokensGatherEachBucketOnce) {
  // Every repeated token names the same bucket; gathering it per token would cost tokens times
  // bucket size before duplicates are removed.
  std::string css;
  for (int i = 0; i < 2000; ++i) {
    css += ".a { fill: red }";
  }
  const css::Stylesheet sheet = Parse(css);
  const RuleIndex index(sheet.rules());
  ASSERT_EQ(index.entryCount(), 2000u);

  RuleIndexElementKeys keys;
  keys.tag = "rect";
  for (int i = 0; i < 2000; ++i) {
    keys.classNames.push_back("a");
  }
  EXPECT_EQ(Gathered(index, keys, 2000), 2000u);
}

TEST(RuleIndexCandidates, AttributeNamesDifferingInCaseGatherTheirBucketOnce) {
  // XML attribute names are case-sensitive, so one element can carry `fill` and `FILL`; both
  // look up the lowercased `[fill]` bucket.
  const RuleIndex index(
      Parse("[fill] { fill: red } [FILL] { fill: red } [Fill] { opacity: 1 }").rules());
  ASSERT_EQ(index.entryCount(), 3u);
  RuleIndexElementKeys keys;
  keys.tag = "rect";
  for (std::string_view name : {"fill", "FILL", "Fill"}) {
    keys.attributeLocalNames.push_back(name);
  }
  EXPECT_EQ(Gathered(index, keys, 3), 3u);
}

TEST(RuleIndexCandidates, GatheringNeverExceedsTheEntryCount) {
  // One rule under every key kind, and an element that carries each key, some several times: the
  // rule is gathered once per entry, never once per key occurrence.
  const css::Stylesheet sheet = Parse("#a, .a, .b, rect, [x], * { fill: red } .a { fill: red }");
  const RuleIndex index(sheet.rules());
  ASSERT_EQ(index.entryCount(), 7u);

  RuleIndexElementKeys keys;
  keys.idName = "a";
  for (std::string_view className : {"a", "b", "a", "b", "a"}) {
    keys.classNames.push_back(className);
  }
  keys.tag = "RECT";
  for (std::string_view name : {"x", "X"}) {
    keys.attributeLocalNames.push_back(name);
  }
  EXPECT_EQ(Gathered(index, keys, 2), index.entryCount());
}

TEST(RuleIndexSpecificity, KeepsRuleAndSelectorListOrder) {
  const css::Stylesheet sheet = Parse(R"css(
      rect, :is(.hot, #id), :where(#ignored) { fill: red }
      :nth-child(2n of g, [title]) { stroke: blue }
  )css");
  ASSERT_THAT(sheet.rules(), ::testing::SizeIs(2));
  ASSERT_THAT(sheet.rules()[0].selector.entries, ::testing::SizeIs(3));
  ASSERT_THAT(sheet.rules()[1].selector.entries, ::testing::SizeIs(1));
  const RuleIndex index(sheet.rules());
  using css::Specificity;
  EXPECT_THAT(Specificity(index.specificity(0, 0)), ::testing::Eq(Specificity::FromABC(0, 0, 1)));
  EXPECT_THAT(Specificity(index.specificity(0, 1)), ::testing::Eq(Specificity::FromABC(1, 0, 0)));
  EXPECT_THAT(Specificity(index.specificity(0, 2)), ::testing::Eq(Specificity::FromABC(0, 0, 0)));
  EXPECT_THAT(Specificity(index.specificity(1, 0)), ::testing::Eq(Specificity::FromABC(0, 2, 0)));
}

TEST(RuleIndexCandidates, EmptyIndex) {
  const RuleIndex index;
  EXPECT_EQ(index.ruleCount(), 0u);
  RuleIndexElementKeys keys;
  keys.tag = "rect";
  EXPECT_THAT(Candidates(index, keys), IsEmpty());
  EXPECT_EQ(index.retainedBytes(), 0u);

  const RuleIndex constructedEmpty(std::span<const css::SelectorRule>{});
  EXPECT_EQ(constructedEmpty.ruleCount(), 0u);
  EXPECT_THAT(Candidates(constructedEmpty, keys), IsEmpty());
  EXPECT_EQ(constructedEmpty.retainedBytes(), 0u);
}

/// The ancestor hashes of the first selector-list entry of `<selector> {}`.
std::vector<std::uint32_t> AncestorHashesOf(std::string_view selector) {
  const css::Stylesheet sheet = Parse(std::string(selector) + " { fill: red }");
  EXPECT_EQ(sheet.rules().size(), 1u) << selector;
  if (sheet.rules().empty()) {
    return {};
  }
  const auto hashes = RuleIndex::AncestorHashesFor(sheet.rules()[0].selector.entries[0]);
  return std::vector<std::uint32_t>(hashes.begin(), hashes.end());
}

std::uint32_t Hash(Kind kind, std::string_view name) {
  return RuleIndex::AncestorKeyHash(kind, name).value();
}

TEST(RuleIndexAncestorHashes, TakeKeysOfCompoundsReachedThroughDescendantAndChildCombinators) {
  EXPECT_THAT(AncestorHashesOf(".a .b"), ElementsAre(Hash(Kind::Class, "a")));
  EXPECT_THAT(AncestorHashesOf("#x > .b"), ElementsAre(Hash(Kind::Id, "x")));
  EXPECT_THAT(AncestorHashesOf(".b"), IsEmpty());
}

TEST(RuleIndexAncestorHashes, IdAndClassKeysKeepTheirCase) {
  EXPECT_THAT(AncestorHashesOf(".A .b"), ElementsAre(Hash(Kind::Class, "A")));
  EXPECT_THAT(AncestorHashesOf("#X .b"), ElementsAre(Hash(Kind::Id, "X")));
}

TEST(RuleIndexAncestorHashes, StopAtTheFirstSiblingCombinator) {
  EXPECT_THAT(AncestorHashesOf(".a + .b"), IsEmpty());
  EXPECT_THAT(AncestorHashesOf(".p .a + .c"), IsEmpty());
  EXPECT_THAT(AncestorHashesOf(".a ~ .b .c"), ElementsAre(Hash(Kind::Class, "b")));
}

TEST(RuleIndexAncestorHashes, PreferIdsThenClassesThenAttributesThenTypesUpToFour) {
  // Ancestors nearest first: `[K]`, `#i`, `g.a`.
  EXPECT_THAT(AncestorHashesOf("g.a #i [K] .b"),
              ElementsAre(Hash(Kind::Id, "i"), Hash(Kind::Class, "a"), Hash(Kind::Attribute, "k"),
                          Hash(Kind::Type, "g")));
  EXPECT_THAT(AncestorHashesOf("#i #j .a .b .c"),
              ElementsAre(Hash(Kind::Id, "j"), Hash(Kind::Id, "i"), Hash(Kind::Class, "b"),
                          Hash(Kind::Class, "a")));
  // Repeated keys count once.
  EXPECT_THAT(AncestorHashesOf(".a .a.b .c"),
              ElementsAre(Hash(Kind::Class, "a"), Hash(Kind::Class, "b")));
}

TEST(RuleIndexAncestorHashes, SkipPseudoClassArgumentsNamespacesAndUniversals) {
  EXPECT_THAT(AncestorHashesOf(":is(.a) .b"), IsEmpty());
  EXPECT_THAT(AncestorHashesOf(".a:not(.z) .b"), ElementsAre(Hash(Kind::Class, "a")));
  EXPECT_THAT(AncestorHashesOf("[*|x] .b"), IsEmpty());
  EXPECT_THAT(AncestorHashesOf("* .b"), IsEmpty());
  EXPECT_THAT(AncestorHashesOf("RECT .b"), ElementsAre(Hash(Kind::Type, "rect")));
}

TEST(RuleIndexAncestorHashes, EachRuleHasOneGroupPerEntryOrNone) {
  const css::Stylesheet sheet = Parse(
      ".a .b { fill: red }"         // 0: one group
      ".x { fill: red }"            // 1: no ancestor key
      ".a .b, .c .d { fill: red }"  // 2: two groups
      ".a .b, .e { fill: red }");   // 3: `.e` can match under any ancestors
  const RuleIndex index(sheet.rules());
  const std::uint32_t a = Hash(Kind::Class, "a");
  const std::uint32_t c = Hash(Kind::Class, "c");
  EXPECT_THAT(index.ancestorHashes(0), ElementsAre(RuleIndex::AncestorHashGroup{a, a, a, a}));
  EXPECT_THAT(index.ancestorHashes(1), IsEmpty());
  EXPECT_THAT(index.ancestorHashes(2), ElementsAre(RuleIndex::AncestorHashGroup{a, a, a, a},
                                                   RuleIndex::AncestorHashGroup{c, c, c, c}));
  EXPECT_THAT(index.ancestorHashes(3), IsEmpty());
}

TEST(RuleIndexAncestorHashes, RetainedBytesCountTheGroupsWithinThePerEntryBound) {
  const RuleIndex child(Parse(".a > .b { fill: red }").rules());
  const RuleIndex sibling(Parse(".a + .b { fill: red }").rules());
  EXPECT_GE(child.retainedBytes() - sibling.retainedBytes(), sizeof(RuleIndex::AncestorHashGroup));

  // Worst case: every entry under its own key, with a hash group.
  std::string css;
  for (int i = 0; i < 1000; ++i) {
    const std::string n = std::to_string(i);
    css += "#p" + n + " #i" + n + ", .q" + n + " #j" + n + " { fill: red }";
  }
  const RuleIndex index(Parse(css).rules());
  ASSERT_EQ(index.entryCount(), 2000u);
  EXPECT_LE(index.retainedBytes(), index.entryCount() * RuleIndex::kMaxBytesPerEntry);
}

}  // namespace
}  // namespace donner::svg::components
