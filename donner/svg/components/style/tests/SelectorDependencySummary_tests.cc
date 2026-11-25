/// @file
/// Which relations a change to each id, class and attribute name reaches, for every selector shape
/// \ref SelectorDependencySummary records, and the selector features it flags.

#include "donner/svg/components/style/SelectorDependencySummary.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::svg::components {
namespace {

constexpr std::uint8_t kSubject = SelectorDependency::Subject;
constexpr std::uint8_t kDescendants = SelectorDependency::Descendants;
constexpr std::uint8_t kFollowing = SelectorDependency::FollowingSiblings;
constexpr std::uint8_t kPreceding = SelectorDependency::PrecedingSiblings;
constexpr std::uint8_t kAncestors = SelectorDependency::Ancestors;

css::Stylesheet Parse(std::string_view css) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  return css::parser::StylesheetParser::Parse(css, warnings);
}

/// The summary of the single rule `<selector> { fill: red }`.
SelectorDependencySummary Summarize(std::string_view selector) {
  const css::Stylesheet sheet = Parse(std::string(selector) + " { fill: red }");
  EXPECT_EQ(sheet.rules().size(), 1u) << selector;
  const css::Stylesheet* sheets[] = {&sheet};
  return SelectorDependencySummary(sheets);
}

/// Relations of the class \p name in the summary of \p selector.
std::uint8_t ClassIn(std::string_view selector, std::string_view name) {
  return Summarize(selector).classDependency(name);
}

TEST(SelectorDependencySummary, SubjectNamesAffectTheElementItself) {
  const SelectorDependencySummary summary = Summarize("rect#x.a[data-k]");
  EXPECT_EQ(summary.idDependency("x"), kSubject);
  EXPECT_EQ(summary.classDependency("a"), kSubject);
  EXPECT_EQ(summary.attributeDependency("data-k"), kSubject);
  EXPECT_EQ(summary.classDependency("b"), 0);
  EXPECT_EQ(summary.idDependency("y"), 0);
  EXPECT_EQ(summary.attributeDependency("fill"), 0);
}

TEST(SelectorDependencySummary, CombinatorsWidenNamesLeftOfThem) {
  EXPECT_EQ(ClassIn(".a .b", "a"), kDescendants);
  EXPECT_EQ(ClassIn(".a .b", "b"), kSubject);
  EXPECT_EQ(ClassIn(".a > .b", "a"), kDescendants);
  EXPECT_EQ(ClassIn(".a + .b", "a"), kFollowing);
  EXPECT_EQ(ClassIn(".a ~ .b", "a"), kFollowing);
  // Each compound takes the combinator immediately to its right.
  EXPECT_EQ(ClassIn(".a .b + .c", "a"), kDescendants);
  EXPECT_EQ(ClassIn(".a .b + .c", "b"), kFollowing);
  EXPECT_EQ(ClassIn(".a + .b .c", "a"), kFollowing);
  EXPECT_EQ(ClassIn(".a + .b .c", "b"), kDescendants);
  EXPECT_EQ(ClassIn(".a + .b .c", "c"), kSubject);
}

TEST(SelectorDependencySummary, IdsClassesAndAttributesCompareAsTheMatcherDoes) {
  const SelectorDependencySummary summary = Summarize("#Id.Cls[viewBox]");
  EXPECT_EQ(summary.idDependency("Id"), kSubject);
  EXPECT_EQ(summary.idDependency("id"), 0);
  EXPECT_EQ(summary.classDependency("Cls"), kSubject);
  EXPECT_EQ(summary.classDependency("cls"), 0);
  // Attribute names over-approximate by ignoring case.
  EXPECT_EQ(summary.attributeDependency("viewBox"), kSubject);
  EXPECT_EQ(summary.attributeDependency("viewbox"), kSubject);
  EXPECT_EQ(summary.attributeDependency("VIEWBOX"), kSubject);
  EXPECT_TRUE(summary.usesAttribute("VIEWBOX"));
  EXPECT_FALSE(summary.usesAttribute("view"));
}

TEST(SelectorDependencySummary, AttributeNamespacesAreIgnored) {
  EXPECT_EQ(Summarize("[*|x]").attributeDependency("x"), kSubject);
  EXPECT_EQ(Summarize("rect[*|href]").attributeDependency("href"), kSubject);
}

TEST(SelectorDependencySummary, PseudoClassArgumentsTakeTheirCompoundsRelations) {
  EXPECT_EQ(ClassIn(":not(.a)", "a"), kSubject);
  EXPECT_EQ(ClassIn(".x:is(.a .b)", "b"), kSubject);
  EXPECT_EQ(ClassIn(".x:is(.a .b)", "a"), kDescendants);
  // Left of a descendant combinator, the argument's subject keeps the compound's relations and a
  // name left of a combinator inside the argument adds that combinator's.
  EXPECT_EQ(ClassIn(":where(.a + .b) .c", "b"), kDescendants);
  EXPECT_EQ(ClassIn(":where(.a + .b) .c", "a"), kDescendants | kFollowing);
  EXPECT_EQ(ClassIn(":is(:not(.a)) ~ .c", "a"), kFollowing);
}

TEST(SelectorDependencySummary, NthChildFiltersReachFollowingSiblings) {
  EXPECT_EQ(ClassIn(":nth-child(1 of .s)", "s"), kSubject | kFollowing);
  EXPECT_EQ(ClassIn("rect:nth-child(2n of .s, .t)", "t"), kSubject | kFollowing);
  EXPECT_EQ(ClassIn(":nth-child(1 of .s) .x", "s"), kDescendants | kFollowing);
  // Names left of a combinator inside the filter widen as anywhere else.
  EXPECT_EQ(ClassIn(":nth-child(1 of :is(.s .t))", "t"), kSubject | kFollowing);
  EXPECT_EQ(ClassIn(":nth-child(1 of :is(.s .t))", "s"), kDescendants | kFollowing);
  EXPECT_EQ(ClassIn(":nth-child(1 of :not(.s))", "s"), kSubject | kFollowing);
}

TEST(SelectorDependencySummary, NthLastChildFiltersReachPrecedingSiblings) {
  EXPECT_EQ(ClassIn(":nth-last-child(1 of .s)", "s"), kSubject | kPreceding);
  EXPECT_EQ(ClassIn(":nth-last-child(2 of .a, .b)", "b"), kSubject | kPreceding);
  EXPECT_EQ(ClassIn(":is(:nth-last-child(1 of .s))", "s"), kSubject | kPreceding);
}

TEST(SelectorDependencySummary, NthFiltersWithSiblingSelectorsIncludeTheEditedElement) {
  EXPECT_EQ(ClassIn("g:nth-last-child(2 of .a + .b)", "a"), kSubject | kPreceding | kFollowing);
  EXPECT_EQ(ClassIn("g:nth-last-child(2 of :is(.a ~ .b))", "a"),
            kSubject | kPreceding | kFollowing);
  EXPECT_EQ(ClassIn("g:nth-child(2 of :not(.a + .b))", "a"), kFollowing);
}

TEST(SelectorDependencySummary, NthFiltersLeftOfSiblingCombinatorsReachBothDirections) {
  EXPECT_EQ(ClassIn(":nth-child(1 of .s) + .x", "s"), kSubject | kFollowing | kPreceding);
  EXPECT_EQ(ClassIn(":nth-last-child(1 of .s) ~ .x", "s"), kSubject | kFollowing | kPreceding);
  // Also when the sibling combinator is outside an enclosing pseudo-class.
  EXPECT_EQ(ClassIn(":is(:nth-last-child(1 of .s)) ~ .x", "s"), kSubject | kFollowing | kPreceding);
  EXPECT_EQ(ClassIn(":is(:nth-child(1 of .s) + .y) .x", "s"),
            kDescendants | kSubject | kFollowing | kPreceding);
}

TEST(SelectorDependencySummary, HasReachesAncestorsAndIsFlagged) {
  const SelectorDependencySummary summary = Summarize("g:has(> .a)");
  EXPECT_TRUE(summary.usesHas());
  EXPECT_EQ(summary.classDependency("a"), kSubject | kAncestors | kPreceding);
  EXPECT_FALSE(Summarize("g:not(.a)").usesHas());
}

TEST(SelectorDependencySummary, FlagsStructuralPseudoClassesAndTextContent) {
  for (std::string_view selector :
       {":first-child", ":last-child", ":only-child", ":nth-child(2)", ":nth-last-child(2)",
        ":nth-of-type(2)", ":nth-last-of-type(2)", ":first-of-type", ":last-of-type",
        ":only-of-type", ":empty", ":not(:first-child)"}) {
    SCOPED_TRACE(selector);
    EXPECT_TRUE(Summarize(selector).usesStructuralPseudoClass());
  }
  for (std::string_view selector : {"rect", ".a > .b", ":not(.a)", ":root"}) {
    SCOPED_TRACE(selector);
    EXPECT_FALSE(Summarize(selector).usesStructuralPseudoClass());
  }
}

TEST(SelectorDependencySummary, DependsOnTextContentOnlyThroughEmpty) {
  for (std::string_view selector :
       {"text", "text:first-child + rect, g > text", "g:has(> text):not(.cls)"}) {
    SCOPED_TRACE(selector);
    EXPECT_FALSE(Summarize(selector).dependsOnTextContent());
  }
  for (std::string_view selector : {"text:empty", "rect, text:EMPTY + rect", "text:not(:empty)",
                                    "g:has(> text:is(.a, :empty))", ":nth-child(2 of :empty)"}) {
    SCOPED_TRACE(selector);
    EXPECT_TRUE(Summarize(selector).dependsOnTextContent());
  }
}

TEST(SelectorDependencySummary, RepeatedNamesCombineTheirRelations) {
  EXPECT_EQ(ClassIn(".a, .a .b", "a"), kSubject | kDescendants);

  const css::Stylesheet first = Parse(".a { fill: red } .b + .c { fill: red }");
  const css::Stylesheet second = Parse(".c .a { fill: red } [x] { fill: red }");
  const css::Stylesheet* sheets[] = {&first, &second};
  const SelectorDependencySummary summary(sheets);
  EXPECT_EQ(summary.classDependency("a"), kSubject);
  EXPECT_EQ(summary.classDependency("b"), kFollowing);
  EXPECT_EQ(summary.classDependency("c"), kSubject | kDescendants);
  EXPECT_EQ(summary.attributeDependency("x"), kSubject);
}

TEST(SelectorDependencySummary, RetainedBytesGrowWithNames) {
  EXPECT_EQ(SelectorDependencySummary().retainedBytes(), 0u);
  EXPECT_EQ(Summarize("rect").retainedBytes(), 0u);
  const std::size_t one = Summarize(".a").retainedBytes();
  EXPECT_GT(one, 0u);
  EXPECT_GT(Summarize(".a .b").retainedBytes(), one);
  // A lowercased attribute name is a copy, and its text counts.
  EXPECT_GT(Summarize("[ABCDEFGHIJKLMNOP]").retainedBytes(), Summarize("[a]").retainedBytes());
}

}  // namespace
}  // namespace donner::svg::components
