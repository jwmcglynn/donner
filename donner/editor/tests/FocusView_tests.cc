#include "donner/editor/FocusView.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "donner/editor/EditorApp.h"
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

SourcePoint PointForNeedle(std::string_view source, std::string_view needle) {
  const std::size_t offset = source.find(needle);
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

  EXPECT_EQ(partition.fullColor, (std::vector<LineRange>{
                                     {.startLine = 2, .endLine = 10},
                                     {.startLine = 11, .endLine = 14},
                                 }));
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
                    .to = PointForNeedle(kReferencedSvg, R"(<linearGradient id="paint")"),
                },
                {
                    .from = PointForNeedle(kReferencedSvg, "#clip"),
                    .to = PointForNeedle(kReferencedSvg, R"(<clipPath id="clip")"),
                },
                {
                    .from = PointForNeedle(kReferencedSvg, "#shadow"),
                    .to = PointForNeedle(kReferencedSvg, R"(<filter id="shadow")"),
                },
                {
                    .from = PointForNeedle(kReferencedSvg, "#base"),
                    .to = PointForNeedle(kReferencedSvg, R"(<linearGradient id="base")"),
                },
            }));
}

TEST(FocusViewTest, IncludesReferencesFromStyleAndDescendantHrefAttributes) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSubtreeReferencedSvg));
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const FocusPartition partition = ComputeFocusPartition(app.document().document(), *target);

  EXPECT_EQ(partition.fullColor, (std::vector<LineRange>{
                                     {.startLine = 2, .endLine = 4},
                                     {.startLine = 5, .endLine = 8},
                                 }));
  EXPECT_EQ(partition.hidden, (std::vector<LineRange>{}));
  EXPECT_EQ(SortLinks(partition.referenceLinks),
            SortLinks(std::vector<FocusReferenceLink>{
                {
                    .from = PointForNeedle(kSubtreeReferencedSvg, "#arrow"),
                    .to = PointForNeedle(kSubtreeReferencedSvg, R"(<marker id="arrow")"),
                },
                {
                    .from = PointForNeedle(kSubtreeReferencedSvg, "#pathRef"),
                    .to = PointForNeedle(kSubtreeReferencedSvg, R"(<path id="pathRef")"),
                },
            }));
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
