/// @file
/// Unit tests for the shared DOM-based id scanning / rewrite helpers.

#include "donner/editor/ElementIdScan.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

namespace {

/// Parse \p svg into a source-backed SVGDocument, asserting success.
svg::SVGDocument Parse(std::string_view svg) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(svg, sink);
  EXPECT_FALSE(result.hasError()) << "parse failed for: " << svg;
  return result.result();
}

constexpr std::string_view kNested =
    R"(<svg xmlns="http://www.w3.org/2000/svg" id="root">
<g id="outer"><rect id="inner" x="0" y="0" width="1" height="1"/><rect x="0" y="0" width="1" height="1"/></g>
</svg>)";

}  // namespace

TEST(ElementIdScan, CollectSubtreeElementsIncludesRootAndDescendants) {
  svg::SVGDocument document = Parse(kNested);
  std::vector<svg::SVGElement> elements;
  CollectSubtreeElements(document.svgElement(), elements);

  // svg, g#outer, rect#inner, and the unnamed rect: four elements total.
  EXPECT_EQ(elements.size(), 4u);
}

TEST(ElementIdScan, CollectSubtreeIdsSkipsEmptyIds) {
  svg::SVGDocument document = Parse(kNested);
  std::unordered_set<std::string> ids;
  CollectSubtreeIds(document.svgElement(), ids);

  EXPECT_EQ(ids, (std::unordered_set<std::string>{"root", "outer", "inner"}));
}

TEST(ElementIdScan, CollectSubtreeIdsFromNonRootStartsAtThatElement) {
  svg::SVGDocument document = Parse(kNested);
  auto outer = document.querySelector("#outer");
  ASSERT_TRUE(outer.has_value());

  std::unordered_set<std::string> ids;
  CollectSubtreeIds(*outer, ids);

  EXPECT_EQ(ids, (std::unordered_set<std::string>{"outer", "inner"}));
}

TEST(ElementIdScan, RewriteIdReferenceInValueRewritesWholeValueHref) {
  const std::optional<std::string> rewritten = RewriteIdReferenceInValue("#old", "old", "new");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, "#new");
}

TEST(ElementIdScan, RewriteIdReferenceInValueRewritesUrlReference) {
  const std::optional<std::string> rewritten =
      RewriteIdReferenceInValue("url(#old)", "old", "new");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, "url(#new)");
}

TEST(ElementIdScan, RewriteIdReferenceInValueIgnoresUnrelatedValue) {
  EXPECT_FALSE(RewriteIdReferenceInValue("url(#other)", "old", "new").has_value());
  EXPECT_FALSE(RewriteIdReferenceInValue("#oldSuffix", "old", "new").has_value());
  EXPECT_FALSE(RewriteIdReferenceInValue("red", "old", "new").has_value());
}

TEST(ElementIdScan, RewriteIdReferenceInValueHandlesMultipleUrlOccurrences) {
  const std::optional<std::string> rewritten =
      RewriteIdReferenceInValue("fill:url(#old);stroke:url(#old)", "old", "new");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, "fill:url(#new);stroke:url(#new)");
}

TEST(ElementIdScan, RewriteIdSelectorInStyleRewritesIdSelector) {
  const std::optional<std::string> rewritten =
      RewriteIdSelectorInStyle("#old { fill: red; }", "old", "new");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, "#new { fill: red; }");
}

TEST(ElementIdScan, RewriteIdSelectorInStyleLeavesHexColorLiteralsAlone) {
  // Inside a declaration value, a bare #token is a color, not an id reference.
  const std::optional<std::string> rewritten =
      RewriteIdSelectorInStyle(".a { fill: #old; }", "old", "new");
  EXPECT_FALSE(rewritten.has_value());
}

TEST(ElementIdScan, RewriteIdSelectorInStyleRewritesUrlReferenceInDeclaration) {
  const std::optional<std::string> rewritten =
      RewriteIdSelectorInStyle(".a { fill: url(#old); }", "old", "new");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, ".a { fill: url(#new); }");
}

TEST(ElementIdScan, RewriteIdSelectorInStyleIgnoresCommentsAndStrings) {
  const std::string_view style = "/* #old */ .a[data-x=\"#old\"] { fill: #old; }";
  EXPECT_FALSE(RewriteIdSelectorInStyle(style, "old", "new").has_value());
}

TEST(ElementIdScan, RewriteIdSelectorInStyleRewritesInsideAtRuleBody) {
  const std::optional<std::string> rewritten =
      RewriteIdSelectorInStyle("@media (min-width: 1px) { #old { fill: red; } }", "old", "new");
  ASSERT_TRUE(rewritten.has_value());
  EXPECT_EQ(*rewritten, "@media (min-width: 1px) { #new { fill: red; } }");
}

}  // namespace donner::editor
