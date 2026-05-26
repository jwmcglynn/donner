#include "donner/editor/FocusView.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/TextEditor.h"
#include "donner/svg/DocumentState.h"
#include "donner/svg/SVGUnknownElement.h"

namespace donner::editor {
namespace {

constexpr std::string_view kNestedSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg">
  <g id="layer">
    <g id="inner">
      <rect id="target" x="1" y="2"/>
      <circle id="sibling"/>
    </g>
  </g>
</svg>)";

constexpr std::string_view kReferencedSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="base">
      <stop offset="0" stop-color="red"/>
    </linearGradient>
    <linearGradient id="paint" href="#base">
      <stop offset="1" stop-color="blue"/>
    </linearGradient>
    <clipPath id="clip"><circle/></clipPath>
    <filter id="shadow"><feDropShadow/></filter>
  </defs>
  <g id="target" fill="url(#paint)" clip-path="url(#clip)" filter="url(#shadow)">
    <rect width="10" height="10"/>
  </g>
  <rect id="sibling"/>
</svg>)svg";

constexpr std::string_view kSubtreeReferencedSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
  <defs>
    <path id="pathRef" d="M0 0H10"/>
    <marker id="arrow"><path d="M0 0L10 5L0 10Z"/></marker>
  </defs>
  <g id="target" style="marker-end: url(#arrow)">
    <use xlink:href="#pathRef"/>
  </g>
</svg>)svg";

constexpr std::string_view kCssReferencedSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .cls-92 { fill: url(#paint); }
    #unused { stroke: red; }
  </style>
  <defs>
    <linearGradient id="paint"><stop offset="0" stop-color="red"/></linearGradient>
  </defs>
  <rect id="target" class="cls-92"/>
  <rect id="sibling"/>
</svg>)svg";

constexpr std::string_view kSelectorListSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .hit, .miss { opacity: 0.5; }
  </style>
  <rect id="hit" class="hit"/>
  <rect id="miss" class="miss"/>
</svg>)svg";

constexpr std::string_view kStyleInDefsSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <defs>
    <style>
      .hit { fill: red; }
    </style>
  </defs>
  <rect id="hit" class="hit"/>
</svg>)svg";

constexpr std::string_view kStyleSourceMutationSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .hit { fill: red; }
  </style>
  <g id="layer">
    <rect id="target" class="hit" x="0" y="0" width="10" height="10"/>
    <rect id="sibling" class="hit" x="20" y="0" width="10" height="10"/>
  </g>
</svg>)svg";

constexpr std::string_view kResourceReferrersSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .from-css { fill: url(#paint); }
    .from-chain { stroke: url(#chained); }
  </style>
  <defs>
    <radialGradient id="paint"><stop offset="1" stop-color="red"/></radialGradient>
    <linearGradient id="chained" href="#paint"><stop offset="1"/></linearGradient>
  </defs>
  <rect id="cssTarget" class="from-css"/>
  <circle id="attrTarget" fill="url(#paint)"/>
  <path id="chainTarget" class="from-chain"/>
  <rect id="sibling"/>
</svg>)svg";

constexpr std::string_view kFilterFanoutSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .uses-other { filter: url(#other); }
  </style>
  <defs>
    <filter id="glow"><feGaussianBlur/></filter>
    <filter id="other" href="#glow"><feOffset/></filter>
  </defs>
  <path id="letterD" filter="url(#glow)"/>
  <path id="otherUser" class="uses-other"/>
</svg>)svg";

constexpr std::string_view kDenseResourceReferrersSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="paint"><stop offset="1"/></linearGradient>
  </defs>
  <rect id="r1" fill="url(#paint)"/>
  <rect id="r2" fill="url(#paint)"/>
  <rect id="r3" fill="url(#paint)"/>
  <rect id="r4" fill="url(#paint)"/>
  <rect id="r5" fill="url(#paint)"/>
  <rect id="r6" fill="url(#paint)"/>
</svg>)svg";

constexpr std::string_view kNonRenderedStopStyleSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    stop { stop-color: red; }
    .target { fill: url(#paint); }
  </style>
  <defs>
    <linearGradient id="paint">
      <stop id="paintStop" offset="0"/>
    </linearGradient>
  </defs>
  <rect id="target" class="target"/>
</svg>)svg";

constexpr std::string_view kUniversalStopStyleSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    * { stop-color: red; }
  </style>
  <defs>
    <linearGradient id="paint">
      <stop id="paintStop" offset="0"/>
    </linearGradient>
  </defs>
</svg>)svg";

constexpr std::string_view kDenseUniversalStyleSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    * { stroke-width: 0; }
  </style>
  <rect id="r1"/>
  <rect id="r2"/>
  <rect id="r3"/>
  <rect id="r4"/>
  <rect id="r5"/>
  <rect id="r6"/>
</svg>)svg";

constexpr std::string_view kGroupedCssChildrenSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .glow { fill: white; }
    .bright { stroke: yellow; }
    .unused { opacity: 0.2; }
  </style>
  <g id="Lightning Glow Bright">
    <path id="boltA" class="glow"/>
    <path id="boltB" class="bright"/>
  </g>
  <path id="sibling" class="unused"/>
</svg>)svg";

constexpr std::string_view kGroupOwnAndChildCssSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .outer { opacity: 0.8; }
    .inner { fill: white; }
  </style>
  <g id="group" class="outer">
    <path id="child" class="inner"/>
  </g>
</svg>)svg";

std::size_t OffsetForNeedle(std::string_view source, std::string_view needle) {
  const std::size_t offset = source.find(needle);
  EXPECT_NE(offset, std::string_view::npos) << needle;
  return offset;
}

SourcePoint PointForNeedle(std::string_view source, std::string_view needle) {
  const std::size_t offset = OffsetForNeedle(source, needle);

  int line = 0;
  std::size_t lineStart = 0;
  for (std::size_t i = 0; i < offset; ++i) {
    if (source[i] == '\n') {
      ++line;
      lineStart = i + 1;
    }
  }

  return SourcePoint{.line = line, .column = static_cast<int>(offset - lineStart)};
}

SourcePoint PointForOpeningTagEnd(std::string_view source, std::string_view openingTagStart) {
  const std::size_t startOffset = OffsetForNeedle(source, openingTagStart);
  const std::size_t closeOffset = source.find('>', startOffset);
  EXPECT_NE(closeOffset, std::string_view::npos) << openingTagStart;
  const std::size_t offset = closeOffset == std::string_view::npos ? startOffset : closeOffset + 1;

  int line = 0;
  std::size_t lineStart = 0;
  for (std::size_t i = 0; i < offset; ++i) {
    if (source[i] == '\n') {
      ++line;
      lineStart = i + 1;
    }
  }

  return SourcePoint{.line = line, .column = static_cast<int>(offset - lineStart)};
}

SourcePoint PointForNeedleAfter(std::string_view source, std::string_view needle,
                                std::string_view after) {
  const std::size_t afterOffset = source.find(after);
  EXPECT_NE(afterOffset, std::string_view::npos) << after;
  const std::size_t offset = source.find(needle, afterOffset + after.size());
  EXPECT_NE(offset, std::string_view::npos) << needle;

  int line = 0;
  std::size_t lineStart = 0;
  for (std::size_t i = 0; i < offset; ++i) {
    if (source[i] == '\n') {
      ++line;
      lineStart = i + 1;
    }
  }

  return SourcePoint{.line = line, .column = static_cast<int>(offset - lineStart)};
}

bool ContainsLine(const std::vector<LineRange>& ranges, int line) {
  return std::ranges::any_of(ranges, [line](const LineRange& range) {
    return line >= range.startLine && line < range.endLine;
  });
}

bool ContainsLink(const std::vector<FocusReferenceLink>& links, const FocusReferenceLink& needle) {
  return std::ranges::find(links, needle) != links.end();
}

bool ContainsElement(const std::vector<svg::SVGElement>& elements,
                     const std::optional<svg::SVGElement>& needle) {
  return needle.has_value() && std::ranges::find(elements, *needle) != elements.end();
}

std::vector<FocusReferenceLink> SortLinks(std::vector<FocusReferenceLink> links) {
  std::sort(links.begin(), links.end(),
            [](const FocusReferenceLink& a, const FocusReferenceLink& b) {
              if (a.from.line != b.from.line) return a.from.line < b.from.line;
              if (a.from.column != b.from.column) return a.from.column < b.from.column;
              if (a.to.line != b.to.line) return a.to.line < b.to.line;
              return a.to.column < b.to.column;
            });
  return links;
}

TEST(FocusViewTest, ShowsSelectedElementAndAncestorTagsOnly) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kNestedSvg));
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *target);

  EXPECT_EQ(partition.fullColor, (std::vector<LineRange>{{.startLine = 3, .endLine = 4}}));
  EXPECT_EQ(partition.dimmed, (std::vector<LineRange>{
                                  {.startLine = 0, .endLine = 3},
                                  {.startLine = 5, .endLine = 8},
                              }));
  EXPECT_EQ(partition.hidden, (std::vector<LineRange>{{.startLine = 4, .endLine = 5}}));
}

TEST(FocusViewTest, IncludesReferencedPaintAndCompositingElements) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kReferencedSvg));
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *target);

  EXPECT_EQ(partition.fullColor, (std::vector<LineRange>{{.startLine = 11, .endLine = 14}}));
  EXPECT_EQ(partition.referenceColor, (std::vector<LineRange>{{.startLine = 2, .endLine = 10}}));
  EXPECT_EQ(partition.dimmed, (std::vector<LineRange>{
                                  {.startLine = 0, .endLine = 2},
                                  {.startLine = 10, .endLine = 11},
                                  {.startLine = 15, .endLine = 16},
                              }));
  EXPECT_EQ(partition.hidden, (std::vector<LineRange>{{.startLine = 14, .endLine = 15}}));
  EXPECT_EQ(SortLinks(partition.referenceLinks),
            SortLinks(std::vector<FocusReferenceLink>{
                {
                    .from = PointForNeedle(kReferencedSvg, "#paint"),
                    .to = PointForOpeningTagEnd(kReferencedSvg, R"(<linearGradient id="paint")"),
                },
                {
                    .from = PointForNeedle(kReferencedSvg, "#clip"),
                    .to = PointForOpeningTagEnd(kReferencedSvg, R"(<clipPath id="clip")"),
                },
                {
                    .from = PointForNeedle(kReferencedSvg, "#shadow"),
                    .to = PointForOpeningTagEnd(kReferencedSvg, R"(<filter id="shadow")"),
                },
                {
                    .from = PointForNeedle(kReferencedSvg, "#base"),
                    .to = PointForOpeningTagEnd(kReferencedSvg, R"(<linearGradient id="base")"),
                },
            }));
}

TEST(FocusViewTest, ReferenceHighlightSummaryCountsForwardReferences) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kReferencedSvg));
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const ReferenceHighlightSummary summary = ComputeReferenceHighlightSummary(
      app.document().document(), std::span<const svg::SVGElement>(&*target, 1));

  EXPECT_EQ(summary.referencedElements.size(), 3u);
  EXPECT_TRUE(ContainsElement(summary.referencedElements,
                              app.document().document().querySelector("#paint")));
  EXPECT_TRUE(ContainsElement(summary.referencedElements,
                              app.document().document().querySelector("#clip")));
  EXPECT_TRUE(ContainsElement(summary.referencedElements,
                              app.document().document().querySelector("#shadow")));
  EXPECT_TRUE(summary.referencingElements.empty());
  EXPECT_EQ(summary.totalCount(), 3u);
}

TEST(FocusViewTest, ReferenceHighlightSummaryAllowsConcurrentDom) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kReferencedSvg));
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const ReferenceHighlightSummary summary = ComputeReferenceHighlightSummary(
      app.document().document(), std::span<const svg::SVGElement>(&*target, 1));

  EXPECT_EQ(summary.referencedElements.size(), 3u);
  EXPECT_TRUE(summary.referencingElements.empty());
}

TEST(FocusViewTest, GroupSelectionDrawsOnlyOwnReferenceArrows) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSubtreeReferencedSvg));
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *target);

  EXPECT_EQ(partition.fullColor, (std::vector<LineRange>{{.startLine = 5, .endLine = 8}}));
  EXPECT_EQ(partition.referenceColor, (std::vector<LineRange>{{.startLine = 2, .endLine = 4}}));
  EXPECT_EQ(partition.hidden, (std::vector<LineRange>{}));
  EXPECT_EQ(SortLinks(partition.referenceLinks),
            SortLinks(std::vector<FocusReferenceLink>{
                {
                    .from = PointForNeedle(kSubtreeReferencedSvg, "#arrow"),
                    .to = PointForOpeningTagEnd(kSubtreeReferencedSvg, R"(<marker id="arrow")"),
                },
            }));
}

TEST(FocusViewTest, IncludesMatchedCssRulesAndCssDeclarationReferences) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCssReferencedSvg));
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *target);

  EXPECT_TRUE(
      ContainsLine(partition.referenceColor, PointForNeedle(kCssReferencedSvg, ".cls-92").line));
  EXPECT_TRUE(ContainsLine(partition.dimmed, PointForNeedle(kCssReferencedSvg, "<style>").line));
  EXPECT_TRUE(ContainsLine(partition.dimmed, PointForNeedle(kCssReferencedSvg, "</style>").line));
  EXPECT_TRUE(
      ContainsLine(partition.referenceColor,
                   PointForNeedle(kCssReferencedSvg, R"(<linearGradient id="paint")").line));
  EXPECT_TRUE(ContainsLine(partition.fullColor,
                           PointForNeedle(kCssReferencedSvg, R"(<rect id="target")").line));
  EXPECT_FALSE(
      ContainsLine(partition.fullColor, PointForNeedle(kCssReferencedSvg, "#unused").line));
  EXPECT_FALSE(
      ContainsLine(partition.referenceColor, PointForNeedle(kCssReferencedSvg, "#unused").line));
  EXPECT_TRUE(ContainsLine(partition.hidden,
                           PointForNeedle(kCssReferencedSvg, R"(<rect id="sibling")").line));

  EXPECT_TRUE(
      ContainsLink(partition.referenceLinks,
                   FocusReferenceLink{
                       .from = PointForOpeningTagEnd(kCssReferencedSvg, R"(<rect id="target")"),
                       .to = PointForNeedle(kCssReferencedSvg, ".cls-92"),
                   }));
  EXPECT_TRUE(ContainsLink(
      partition.referenceLinks,
      FocusReferenceLink{
          .from = PointForNeedle(kCssReferencedSvg, "#paint"),
          .to = PointForOpeningTagEnd(kCssReferencedSvg, R"(<linearGradient id="paint")"),
      }));
}

TEST(FocusViewTest, StyleOffsetFocusIncludesImpactedElementsAndCssDeclarationReferences) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kCssReferencedSvg));

  const std::optional<FocusPartition> partition = ComputeStyleFocusPartitionAtSourceOffset(
      app.document().document(), OffsetForNeedle(kCssReferencedSvg, ".cls-92"));
  ASSERT_TRUE(partition.has_value());

  EXPECT_TRUE(
      ContainsLine(partition->fullColor, PointForNeedle(kCssReferencedSvg, ".cls-92").line));
  EXPECT_TRUE(ContainsLine(partition->dimmed, PointForNeedle(kCssReferencedSvg, "<style>").line));
  EXPECT_TRUE(ContainsLine(partition->dimmed, PointForNeedle(kCssReferencedSvg, "</style>").line));
  EXPECT_FALSE(ContainsLine(partition->hidden, PointForNeedle(kCssReferencedSvg, "<style>").line));
  EXPECT_FALSE(ContainsLine(partition->hidden, PointForNeedle(kCssReferencedSvg, "</style>").line));
  EXPECT_TRUE(
      ContainsLine(partition->referenceColor,
                   PointForNeedle(kCssReferencedSvg, R"(<linearGradient id="paint")").line));
  EXPECT_TRUE(ContainsLine(partition->referenceColor,
                           PointForNeedle(kCssReferencedSvg, R"(<rect id="target")").line));
  EXPECT_TRUE(ContainsLine(partition->hidden,
                           PointForNeedle(kCssReferencedSvg, R"(<rect id="sibling")").line));

  EXPECT_TRUE(
      ContainsLink(partition->referenceLinks,
                   FocusReferenceLink{
                       .from = PointForOpeningTagEnd(kCssReferencedSvg, R"(<rect id="target")"),
                       .to = PointForNeedle(kCssReferencedSvg, ".cls-92"),
                   }));
  EXPECT_FALSE(ContainsLink(partition->referenceLinks,
                            FocusReferenceLink{
                                .from = PointForNeedle(kCssReferencedSvg, ".cls-92"),
                                .to = PointForNeedle(kCssReferencedSvg, R"(<rect id="target")"),
                            }));
  EXPECT_TRUE(ContainsLink(
      partition->referenceLinks,
      FocusReferenceLink{
          .from = PointForNeedle(kCssReferencedSvg, "#paint"),
          .to = PointForOpeningTagEnd(kCssReferencedSvg, R"(<linearGradient id="paint")"),
      }));
}

TEST(FocusViewTest, StyleOffsetFocusUsesSelectorListBranchUnderCursor) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSelectorListSvg));

  const std::optional<FocusPartition> hitPartition = ComputeStyleFocusPartitionAtSourceOffset(
      app.document().document(), OffsetForNeedle(kSelectorListSvg, ".hit"));
  ASSERT_TRUE(hitPartition.has_value());
  EXPECT_TRUE(ContainsLine(hitPartition->referenceColor,
                           PointForNeedle(kSelectorListSvg, R"(<rect id="hit")").line));
  EXPECT_TRUE(ContainsLine(hitPartition->hidden,
                           PointForNeedle(kSelectorListSvg, R"(<rect id="miss")").line));

  const std::optional<FocusPartition> blockPartition = ComputeStyleFocusPartitionAtSourceOffset(
      app.document().document(), OffsetForNeedle(kSelectorListSvg, "opacity"));
  ASSERT_TRUE(blockPartition.has_value());
  EXPECT_TRUE(ContainsLine(blockPartition->referenceColor,
                           PointForNeedle(kSelectorListSvg, R"(<rect id="hit")").line));
  EXPECT_TRUE(ContainsLine(blockPartition->referenceColor,
                           PointForNeedle(kSelectorListSvg, R"(<rect id="miss")").line));
}

TEST(FocusViewTest, StyleOffsetFocusShowsStyleElementAncestors) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStyleInDefsSvg));

  const std::optional<FocusPartition> partition = ComputeStyleFocusPartitionAtSourceOffset(
      app.document().document(), OffsetForNeedle(kStyleInDefsSvg, ".hit"));
  ASSERT_TRUE(partition.has_value());

  EXPECT_TRUE(ContainsLine(partition->dimmed, PointForNeedle(kStyleInDefsSvg, "<svg").line));
  EXPECT_TRUE(ContainsLine(partition->dimmed, PointForNeedle(kStyleInDefsSvg, "<defs>").line));
  EXPECT_TRUE(ContainsLine(partition->dimmed, PointForNeedle(kStyleInDefsSvg, "<style>").line));
  EXPECT_TRUE(ContainsLine(partition->dimmed, PointForNeedle(kStyleInDefsSvg, "</style>").line));
  EXPECT_TRUE(ContainsLine(partition->dimmed, PointForNeedle(kStyleInDefsSvg, "</defs>").line));
  EXPECT_TRUE(ContainsLine(partition->dimmed, PointForNeedle(kStyleInDefsSvg, "</svg>").line));
  EXPECT_FALSE(ContainsLine(partition->hidden, PointForNeedle(kStyleInDefsSvg, "<defs>").line));
  EXPECT_FALSE(ContainsLine(partition->hidden, PointForNeedle(kStyleInDefsSvg, "</defs>").line));
}

TEST(FocusViewTest, StyleFocusReportsImpactedElementsForCanvasSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSelectorListSvg));

  const std::optional<StyleFocus> hitFocus = ComputeStyleFocusAtSourceOffset(
      app.document().document(), OffsetForNeedle(kSelectorListSvg, ".hit"));
  ASSERT_TRUE(hitFocus.has_value());
  ASSERT_EQ(hitFocus->impactedElements.size(), 1u);
  EXPECT_EQ(hitFocus->impactedElements[0].id(), "hit");

  const std::optional<StyleFocus> blockFocus = ComputeStyleFocusAtSourceOffset(
      app.document().document(), OffsetForNeedle(kSelectorListSvg, "opacity"));
  ASSERT_TRUE(blockFocus.has_value());
  ASSERT_EQ(blockFocus->impactedElements.size(), 2u);
  EXPECT_EQ(blockFocus->impactedElements[0].id(), "hit");
  EXPECT_EQ(blockFocus->impactedElements[1].id(), "miss");
}

TEST(FocusViewTest, StyleOffsetFocusSurvivesStructuredSourceTypingMutations) {
  struct MutationCase {
    std::string_view name;
    std::string_view oldText;
    std::string_view newText;
    bool expectParseError = false;
  };

  const MutationCase cases[] = {
      {
          .name = "replace styled element with same id and class",
          .oldText = R"(<rect id="target" class="hit" x="0" y="0" width="10" height="10"/>)",
          .newText = R"(<rect id="target" class="hit" x="2" y="3" width="12" height="14"/>)",
      },
      {
          .name = "remove styled element attribute",
          .oldText = R"( x="0")",
          .newText = "",
      },
      {
          .name = "delete styled sibling element",
          .oldText = R"(<rect id="sibling" class="hit" x="20" y="0" width="10" height="10"/>)",
          .newText = "",
      },
      {
          .name = "type malformed selected element attribute",
          .oldText = R"(width="10")",
          .newText = R"(width="10)",
          .expectParseError = true,
      },
  };

  for (const MutationCase& mutationCase : cases) {
    SCOPED_TRACE(mutationCase.name);

    EditorApp app;
    app.setStructuredEditingEnabled(true);
    ASSERT_TRUE(app.loadFromString(kStyleSourceMutationSvg));

    TextEditor textEditor;
    textEditor.setText(kStyleSourceMutationSvg);
    textEditor.resetTextChanged();
    DocumentSyncController controller{std::string(kStyleSourceMutationSvg)};

    std::optional<svg::SVGElement> selected = app.document().document().querySelector("#target");
    ASSERT_TRUE(selected.has_value());
    app.setSelection(*selected);

    const std::string currentText = textEditor.getText();
    const std::size_t editOffset = currentText.find(mutationCase.oldText);
    ASSERT_NE(editOffset, std::string::npos);

    textEditor.setSelection(
        textEditor.getCoordinatesAtByteOffset(editOffset),
        textEditor.getCoordinatesAtByteOffset(editOffset + mutationCase.oldText.size()));
    textEditor.insertText(mutationCase.newText);
    controller.handleTextEdits(app, textEditor, /*deltaSeconds=*/0.0f);

    const std::string editedSource = textEditor.getText();
    const std::optional<StyleFocus> focus = ComputeStyleFocusAtSourceOffset(
        app.document().document(), OffsetForNeedle(editedSource, ".hit"));
    if (!mutationCase.expectParseError) {
      ASSERT_TRUE(focus.has_value());
      EXPECT_FALSE(focus->impactedElements.empty());
    } else {
      EXPECT_TRUE(app.document().lastParseError().has_value());
    }
  }
}

TEST(FocusViewTest, SelectingGroupShowsDescendantCssRulesWithoutDescendantLinks) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kGroupedCssChildrenSvg));
  std::optional<svg::SVGElement> group =
      app.document().document().querySelector("#Lightning\\ Glow\\ Bright");
  ASSERT_TRUE(group.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *group);

  EXPECT_TRUE(
      ContainsLine(partition.referenceColor, PointForNeedle(kGroupedCssChildrenSvg, ".glow").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kGroupedCssChildrenSvg, ".bright").line));
  EXPECT_FALSE(
      ContainsLine(partition.fullColor, PointForNeedle(kGroupedCssChildrenSvg, ".unused").line));
  EXPECT_FALSE(ContainsLine(partition.referenceColor,
                            PointForNeedle(kGroupedCssChildrenSvg, ".unused").line));
  EXPECT_TRUE(ContainsLine(partition.hidden,
                           PointForNeedle(kGroupedCssChildrenSvg, R"(<path id="sibling")").line));

  EXPECT_TRUE(partition.referenceLinks.empty());
}

TEST(FocusViewTest, SelectingGroupStillDrawsOwnCssRuleLink) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kGroupOwnAndChildCssSvg));
  std::optional<svg::SVGElement> group = app.document().document().querySelector("#group");
  ASSERT_TRUE(group.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *group);

  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kGroupOwnAndChildCssSvg, ".outer").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kGroupOwnAndChildCssSvg, ".inner").line));
  EXPECT_EQ(SortLinks(partition.referenceLinks),
            SortLinks(std::vector<FocusReferenceLink>{
                {
                    .from = PointForOpeningTagEnd(kGroupOwnAndChildCssSvg, R"(<g id="group")"),
                    .to = PointForNeedle(kGroupOwnAndChildCssSvg, ".outer"),
                },
            }));
}

TEST(FocusViewTest, SelectingMultipleElementsIncludesCssRuleLinksForEachSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kGroupedCssChildrenSvg));
  std::optional<svg::SVGElement> boltA = app.document().document().querySelector("#boltA");
  std::optional<svg::SVGElement> boltB = app.document().document().querySelector("#boltB");
  ASSERT_TRUE(boltA.has_value());
  ASSERT_TRUE(boltB.has_value());

  const std::vector<svg::SVGElement> selection{*boltA, *boltB};
  const FocusPartition partition = ComputeFocusPartition(app.document().document(), selection);

  EXPECT_TRUE(ContainsLine(partition.fullColor,
                           PointForNeedle(kGroupedCssChildrenSvg, R"(<path id="boltA")").line));
  EXPECT_TRUE(ContainsLine(partition.fullColor,
                           PointForNeedle(kGroupedCssChildrenSvg, R"(<path id="boltB")").line));
  EXPECT_TRUE(
      ContainsLine(partition.referenceColor, PointForNeedle(kGroupedCssChildrenSvg, ".glow").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kGroupedCssChildrenSvg, ".bright").line));
  EXPECT_TRUE(
      ContainsLink(partition.referenceLinks,
                   FocusReferenceLink{
                       .from = PointForOpeningTagEnd(kGroupedCssChildrenSvg, R"(<path id="boltA")"),
                       .to = PointForNeedle(kGroupedCssChildrenSvg, ".glow"),
                   }));
  EXPECT_TRUE(
      ContainsLink(partition.referenceLinks,
                   FocusReferenceLink{
                       .from = PointForOpeningTagEnd(kGroupedCssChildrenSvg, R"(<path id="boltB")"),
                       .to = PointForNeedle(kGroupedCssChildrenSvg, ".bright"),
                   }));
}

TEST(FocusViewTest, SelectingReferencedResourceIncludesReferringElementsAndCssRules) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kResourceReferrersSvg));
  std::optional<svg::SVGElement> paint = app.document().document().querySelector("#paint");
  ASSERT_TRUE(paint.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *paint);

  EXPECT_TRUE(
      ContainsLine(partition.fullColor,
                   PointForNeedle(kResourceReferrersSvg, R"(<radialGradient id="paint")").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kResourceReferrersSvg, ".from-css").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kResourceReferrersSvg, ".from-chain").line));
  EXPECT_TRUE(
      ContainsLine(partition.referenceColor,
                   PointForNeedle(kResourceReferrersSvg, R"(<linearGradient id="chained")").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kResourceReferrersSvg, R"(<rect id="cssTarget")").line));
  EXPECT_TRUE(
      ContainsLine(partition.referenceColor,
                   PointForNeedle(kResourceReferrersSvg, R"(<circle id="attrTarget")").line));
  EXPECT_TRUE(
      ContainsLine(partition.referenceColor,
                   PointForNeedle(kResourceReferrersSvg, R"(<path id="chainTarget")").line));
  EXPECT_TRUE(ContainsLine(partition.hidden,
                           PointForNeedle(kResourceReferrersSvg, R"(<rect id="sibling")").line));

  EXPECT_TRUE(ContainsLink(
      partition.referenceLinks,
      FocusReferenceLink{
          .from = PointForNeedle(kResourceReferrersSvg, "#paint"),
          .to = PointForOpeningTagEnd(kResourceReferrersSvg, R"(<radialGradient id="paint")"),
      }));
  EXPECT_TRUE(ContainsLink(
      partition.referenceLinks,
      FocusReferenceLink{
          .from = PointForNeedleAfter(kResourceReferrersSvg, "#paint",
                                      R"(<linearGradient id="chained")"),
          .to = PointForOpeningTagEnd(kResourceReferrersSvg, R"(<radialGradient id="paint")"),
      }));
  EXPECT_TRUE(ContainsLink(
      partition.referenceLinks,
      FocusReferenceLink{
          .from =
              PointForNeedleAfter(kResourceReferrersSvg, "#paint", R"(<circle id="attrTarget")"),
          .to = PointForOpeningTagEnd(kResourceReferrersSvg, R"(<radialGradient id="paint")"),
      }));
  EXPECT_TRUE(ContainsLink(
      partition.referenceLinks,
      FocusReferenceLink{
          .from = PointForNeedle(kResourceReferrersSvg, "#chained"),
          .to = PointForOpeningTagEnd(kResourceReferrersSvg, R"(<linearGradient id="chained")"),
      }));
}

TEST(FocusViewTest, ReferenceHighlightSummaryCountsReverseReferences) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kResourceReferrersSvg));
  std::optional<svg::SVGElement> paint = app.document().document().querySelector("#paint");
  ASSERT_TRUE(paint.has_value());

  const ReferenceHighlightSummary summary = ComputeReferenceHighlightSummary(
      app.document().document(), std::span<const svg::SVGElement>(&*paint, 1));

  EXPECT_TRUE(summary.referencedElements.empty());
  EXPECT_EQ(summary.referencingElements.size(), 3u);
  EXPECT_TRUE(ContainsElement(summary.referencingElements,
                              app.document().document().querySelector("#cssTarget")));
  EXPECT_TRUE(ContainsElement(summary.referencingElements,
                              app.document().document().querySelector("#attrTarget")));
  EXPECT_TRUE(ContainsElement(summary.referencingElements,
                              app.document().document().querySelector("#chained")));
  EXPECT_EQ(summary.totalCount(), 3u);
}

TEST(FocusViewTest, SuppressesReverseReferenceExpansionAfterFiveRefs) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kDenseResourceReferrersSvg));
  std::optional<svg::SVGElement> paint = app.document().document().querySelector("#paint");
  ASSERT_TRUE(paint.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *paint);

  EXPECT_TRUE(ContainsLine(
      partition.fullColor,
      PointForNeedle(kDenseResourceReferrersSvg, R"(<linearGradient id="paint")").line));
  EXPECT_FALSE(ContainsLine(partition.referenceColor,
                            PointForNeedle(kDenseResourceReferrersSvg, R"(<rect id="r1")").line));
  EXPECT_FALSE(ContainsLine(partition.referenceColor,
                            PointForNeedle(kDenseResourceReferrersSvg, R"(<rect id="r6")").line));
  EXPECT_TRUE(ContainsLine(partition.hidden,
                           PointForNeedle(kDenseResourceReferrersSvg, R"(<rect id="r1")").line));
  EXPECT_TRUE(partition.referenceLinks.empty());
}

TEST(FocusViewTest, StyleFocusSuppressesUniversalSelectorExpansionAfterFiveRefs) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kDenseUniversalStyleSvg));

  const std::optional<StyleFocus> focus = ComputeStyleFocusAtSourceOffset(
      app.document().document(), OffsetForNeedle(kDenseUniversalStyleSvg, "stroke-width"));
  ASSERT_TRUE(focus.has_value());

  EXPECT_TRUE(focus->reverseReferenceExpansionSuppressed);
  EXPECT_TRUE(focus->impactedElements.empty());
  EXPECT_TRUE(ContainsLine(focus->partition.fullColor,
                           PointForNeedle(kDenseUniversalStyleSvg, "* {").line));
  EXPECT_FALSE(ContainsLine(focus->partition.referenceColor,
                            PointForNeedle(kDenseUniversalStyleSvg, R"(<rect id="r1")").line));
  EXPECT_FALSE(ContainsLine(focus->partition.referenceColor,
                            PointForNeedle(kDenseUniversalStyleSvg, R"(<rect id="r6")").line));
  EXPECT_TRUE(focus->partition.referenceLinks.empty());
}

TEST(FocusViewTest, ForwardFilterDependencyDoesNotReverseExpandOtherFilterReferrers) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kFilterFanoutSvg));
  std::optional<svg::SVGElement> letterD = app.document().document().querySelector("#letterD");
  ASSERT_TRUE(letterD.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *letterD);

  EXPECT_TRUE(ContainsLine(partition.fullColor,
                           PointForNeedle(kFilterFanoutSvg, R"(<path id="letterD")").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kFilterFanoutSvg, R"(<filter id="glow")").line));
  EXPECT_FALSE(ContainsLine(partition.fullColor,
                            PointForNeedle(kFilterFanoutSvg, R"(<filter id="other")").line));
  EXPECT_FALSE(ContainsLine(partition.referenceColor,
                            PointForNeedle(kFilterFanoutSvg, R"(<filter id="other")").line));
  EXPECT_FALSE(
      ContainsLine(partition.fullColor, PointForNeedle(kFilterFanoutSvg, ".uses-other").line));
  EXPECT_FALSE(
      ContainsLine(partition.referenceColor, PointForNeedle(kFilterFanoutSvg, ".uses-other").line));
  EXPECT_FALSE(ContainsLine(partition.fullColor,
                            PointForNeedle(kFilterFanoutSvg, R"(<path id="otherUser")").line));
  EXPECT_FALSE(ContainsLine(partition.referenceColor,
                            PointForNeedle(kFilterFanoutSvg, R"(<path id="otherUser")").line));

  EXPECT_TRUE(ContainsLink(
      partition.referenceLinks,
      FocusReferenceLink{
          .from = PointForNeedleAfter(kFilterFanoutSvg, "#glow", R"(<path id="letterD")"),
          .to = PointForOpeningTagEnd(kFilterFanoutSvg, R"(<filter id="glow")"),
      }));
}

TEST(FocusViewTest, SelectingFilterResourceStillIncludesTransitiveReferrers) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kFilterFanoutSvg));
  std::optional<svg::SVGElement> glow = app.document().document().querySelector("#glow");
  ASSERT_TRUE(glow.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *glow);

  EXPECT_TRUE(ContainsLine(partition.fullColor,
                           PointForNeedle(kFilterFanoutSvg, R"(<filter id="glow")").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kFilterFanoutSvg, R"(<filter id="other")").line));
  EXPECT_TRUE(
      ContainsLine(partition.referenceColor, PointForNeedle(kFilterFanoutSvg, ".uses-other").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kFilterFanoutSvg, R"(<path id="otherUser")").line));

  EXPECT_TRUE(ContainsLink(
      partition.referenceLinks,
      FocusReferenceLink{
          .from = PointForNeedleAfter(kFilterFanoutSvg, "#glow", R"(<filter id="other")"),
          .to = PointForOpeningTagEnd(kFilterFanoutSvg, R"(<filter id="glow")"),
      }));
  EXPECT_TRUE(
      ContainsLink(partition.referenceLinks,
                   FocusReferenceLink{
                       .from = PointForNeedle(kFilterFanoutSvg, "#other"),
                       .to = PointForOpeningTagEnd(kFilterFanoutSvg, R"(<filter id="other")"),
                   }));
}

TEST(FocusViewTest, ReferencedGradientOmitsCssRulesMatchedOnlyByStopDescendants) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kNonRenderedStopStyleSvg));
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *target);

  EXPECT_TRUE(
      ContainsLine(partition.referenceColor,
                   PointForNeedle(kNonRenderedStopStyleSvg, R"(<linearGradient id="paint")").line));
  EXPECT_TRUE(ContainsLine(partition.referenceColor,
                           PointForNeedle(kNonRenderedStopStyleSvg, ".target").line));
  EXPECT_FALSE(
      ContainsLine(partition.fullColor, PointForNeedle(kNonRenderedStopStyleSvg, "stop {").line));
  EXPECT_FALSE(ContainsLine(partition.referenceColor,
                            PointForNeedle(kNonRenderedStopStyleSvg, "stop {").line));
}

TEST(FocusViewTest, SelectingStopOmitsUniversalSelectorProvenance) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kUniversalStopStyleSvg));
  std::optional<svg::SVGElement> stop = app.document().document().querySelector("#paintStop");
  ASSERT_TRUE(stop.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *stop);

  EXPECT_TRUE(ContainsLine(partition.fullColor,
                           PointForNeedle(kUniversalStopStyleSvg, R"(<stop id="paintStop")").line));
  EXPECT_FALSE(
      ContainsLine(partition.fullColor, PointForNeedle(kUniversalStopStyleSvg, "* {").line));
  EXPECT_FALSE(
      ContainsLine(partition.referenceColor, PointForNeedle(kUniversalStopStyleSvg, "* {").line));
}

TEST(FocusViewTest, ReturnsEmptyPartitionWithoutSourceLocations) {
  svg::SVGDocument document;
  svg::SVGSVGElement root = document.svgElement();
  svg::SVGElement child = svg::SVGUnknownElement::Create(document, "g");
  root.appendChild(child);

  EXPECT_TRUE(ComputeFocusPartition(document, child).empty());
}

}  // namespace
}  // namespace donner::editor
