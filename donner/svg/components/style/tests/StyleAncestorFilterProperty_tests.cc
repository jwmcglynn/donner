/// @file
/// The ancestor filter never rules out a rule that matches: for seeded random documents, with
/// `<use>` shadow trees, and random selectors, every element the cascade matches has a hash group
/// of the rule whose hashes the filter, synchronized to that element, may all contain.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/CSS.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/RuleIndex.h"
#include "donner/svg/components/style/StyleAncestorFilter.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg::components {
namespace {

/// Seeded generator of small documents and selectors over a shared vocabulary of ids, classes
/// in both cases, attribute names in both cases, and `<use>` targets.
class DocumentGenerator {
public:
  explicit DocumentGenerator(unsigned seed) : engine_(seed) {}

  /// A document whose stylesheet is \p selector with one declaration.
  std::string document(const std::string& selector) {
    return R"(<svg xmlns="http://www.w3.org/2000/svg")" + attributes() + "><style>" + selector +
           " { stop-color: green }</style><defs><g id=\"t1\"" + attributes() + ">" + subtree(2) +
           subtree(1) + "</g><g" + attributes() + "><symbol id=\"t2\"" + attributes() + ">" +
           subtree(2) + "</symbol></g></defs>" + subtree(4) + subtree(3) + "</svg>";
  }

  /// One or two complex selectors.
  std::string selector() {
    std::string result = complex();
    if (chance(20)) {
      result += ", " + complex();
    }
    return result;
  }

private:
  int pick(int count) { return std::uniform_int_distribution<int>(0, count - 1)(engine_); }
  bool chance(int percent) { return pick(100) < percent; }
  std::string_view of(std::span<const std::string_view> items) {
    return items[pick(static_cast<int>(items.size()))];
  }

  std::string classList() {
    static constexpr std::string_view kClasses[] = {"a", "A", "b", "B", "c"};
    static constexpr std::string_view kSeparators[] = {" ", "\t", "\n", "  "};
    std::string result(of(kClasses));
    for (int count = pick(3); count > 0; --count) {
      result += std::string(of(kSeparators)) + std::string(of(kClasses));
    }
    return result;
  }

  std::string attributes() {
    static constexpr std::string_view kIds[] = {"a", "A", "b", "x"};
    static constexpr std::string_view kExtra[] = {R"(data-x="1")",    R"(Data-Y="2")",
                                                  R"(fill="red")",    R"(fill="RED")",
                                                  R"(stroke="blue")", R"(xml:space="preserve")"};
    std::string result;
    if (chance(30)) {
      result += " id=\"" + std::string(of(kIds)) + "\"";
    }
    if (chance(60)) {
      result += " class=\"" + classList() + "\"";
    }
    if (chance(40)) {
      result += " " + std::string(of(kExtra));
    }
    return result;
  }

  std::string leaf() {
    static constexpr std::string_view kLeaves[] = {"rect", "circle", "rect"};
    if (!chance(25)) {
      return "<" + std::string(of(kLeaves)) + attributes() + R"( width="1" height="1"/>)";
    }
    const std::string use =
        std::string(R"(<use href="#)") + (chance(50) ? "t1" : "t2") + "\"" + attributes();
    return chance(40) ? use + "><desc" + attributes() + "/><title" + attributes() + "/></use>"
                      : use + "/>";
  }

  std::string subtree(int depth) {
    static constexpr std::string_view kContainers[] = {"g", "svg", "g", "symbol"};
    if (depth <= 0 || chance(35)) {
      return leaf();
    }
    const std::string tag(of(kContainers));
    std::string result = "<" + tag + attributes() + ">";
    for (int count = 1 + pick(3); count > 0; --count) {
      result += subtree(depth - 1);
    }
    return result + "</" + tag + ">";
  }

  std::string pseudoClass(int depth) {
    switch (pick(5)) {
      case 0: return ":not(" + simple(depth - 1) + ")";
      case 1: return ":is(" + simple(depth - 1) + ", " + simple(depth - 1) + ")";
      case 2: return ":where(" + simple(depth - 1) + ")";
      case 3: return ":has(> " + simple(depth - 1) + ")";
      default: return ":has(" + simple(depth - 1) + " " + simple(depth - 1) + ")";
    }
  }

  std::string simple(int depth) {
    static constexpr std::string_view kSimple[] = {"#a",
                                                   "#A",
                                                   "#b",
                                                   "#x",
                                                   "#t1",
                                                   ".a",
                                                   ".A",
                                                   ".b",
                                                   ".B",
                                                   ".c",
                                                   "[data-x]",
                                                   "[DATA-X]",
                                                   "[data-y]",
                                                   "[Data-Y]",
                                                   "[fill]",
                                                   "[FILL]",
                                                   "[fill=red i]",
                                                   "[fill=red]",
                                                   "[stroke]",
                                                   "[class~=a]",
                                                   "[id]",
                                                   "[space]",
                                                   "[xml|space]",
                                                   ":first-child",
                                                   ":last-child",
                                                   ":only-child",
                                                   ":nth-child(2n+1)",
                                                   ":nth-of-type(1)",
                                                   ":root",
                                                   ":empty"};
    if (depth > 0 && chance(25)) {
      return pseudoClass(depth);
    }
    return std::string(of(kSimple));
  }

  std::string compound() {
    static constexpr std::string_view kTypes[] = {"g",     "G",   "rect", "RECT",   "circle",
                                                  "svg",   "Svg", "use",  "symbol", "desc",
                                                  "title", "*",   "*|g",  "svg|g"};
    static constexpr std::string_view kCommon[] = {".a", ".b", ".A", "[fill]", "[data-x]", "#a"};
    std::string result;
    if (chance(45)) {
      result += of(kTypes);
    }
    if (result.empty() || chance(30)) {
      result += chance(60) ? std::string(of(kCommon)) : simple(1);
    }
    if (chance(15)) {
      result += simple(1);
    }
    return result;
  }

  std::string complex() {
    static constexpr std::string_view kCombinators[] = {" ", " > ", " ", " ", " > ", " + ", " ~ "};
    std::string result = compound();
    for (int count = pick(4); count > 0; --count) {
      result += std::string(of(kCombinators)) + compound();
    }
    return result;
  }

  std::mt19937 engine_;
};

/// What one document contributed.
struct Tally {
  int matchedWithGroups = 0;  //!< Matched elements whose rule has hash groups.
  int rejectedUnmatched = 0;  //!< Unmatched elements the filter would have rejected.
  int falseNegatives = 0;     //!< Matched elements the filter would have rejected.

  void record(bool matched, bool mayMatch, bool hasGroups) {
    falseNegatives += matched && !mayMatch ? 1 : 0;
    matchedWithGroups += matched && hasGroups ? 1 : 0;
    rejectedUnmatched += !matched && !mayMatch ? 1 : 0;
  }
};

/// Whether the filter lets a rule with \p groups through: it has no groups, or some group's
/// hashes may all be present.
bool MayMatch(const StyleAncestorFilter& filter,
              std::span<const RuleIndex::AncestorHashGroup> groups) {
  if (groups.empty()) {
    return true;
  }
  for (const RuleIndex::AncestorHashGroup& group : groups) {
    bool all = true;
    for (const std::uint32_t hash : group) {
      all = all && filter.mayContain(BloomHash(hash));
    }
    if (all) {
      return true;
    }
  }
  return false;
}

/// Style the document of \p seed and compare every element's match with the filter's answer.
std::optional<Tally> CheckSeed(unsigned seed) {
  DocumentGenerator generator(seed);
  const std::string selector = generator.selector();
  const std::string source = generator.document(selector);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  const css::Stylesheet sheet =
      css::CSS::ParseStylesheet(selector + " { stop-color: green }", warnings);
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  if (sheet.rules().size() != 1 || parsed.hasError()) {
    return std::nullopt;
  }
  SVGDocument document = std::move(parsed.result());
  Registry& registry = document.registry();
  RenderingContext(registry).instantiateRenderTree(false, warnings);

  const RuleIndex index(sheet.rules());
  const std::span<const RuleIndex::AncestorHashGroup> groups = index.ancestorHashes(0);
  const std::optional<css::Color> green = css::Color(css::RGBA(0, 0x80, 0, 0xFF));
  StyleAncestorFilter filter(registry);
  Tally tally;
  for (const auto& [entity, style] : registry.view<ComputedStyleComponent>().each()) {
    const PropertyRegistry* properties = style.properties();
    if (properties == nullptr || !registry.all_of<donner::components::TreeComponent>(entity)) {
      continue;
    }
    filter.synchronizeTo(entity);
    const bool mayMatch = MayMatch(filter, groups);
    const bool matched = properties->stopColor.get() == green;
    tally.record(matched, mayMatch, !groups.empty());
    EXPECT_TRUE(!matched || mayMatch) << "seed " << seed << " selector [" << selector << "]\n"
                                      << source;
  }
  return tally;
}

TEST(StyleAncestorFilterProperty, FilterRejectsOnlyRulesTheCascadeDoesNotMatch) {
  constexpr unsigned kSeeds = 300;
  Tally total;
  for (unsigned seed = 1; seed <= kSeeds; ++seed) {
    if (const std::optional<Tally> tally = CheckSeed(seed)) {
      total.matchedWithGroups += tally->matchedWithGroups;
      total.rejectedUnmatched += tally->rejectedUnmatched;
      total.falseNegatives += tally->falseNegatives;
    }
  }
  EXPECT_EQ(total.falseNegatives, 0);
  // Not vacuous: 156 matches with hash groups and 4,263 rejections at the time of writing.
  EXPECT_GT(total.matchedWithGroups, 75);
  EXPECT_GT(total.rejectedUnmatched, 2000);
  RecordProperty("matched_with_groups", total.matchedWithGroups);
  RecordProperty("rejected_unmatched", total.rejectedUnmatched);
}

}  // namespace
}  // namespace donner::svg::components
