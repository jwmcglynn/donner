#include "donner/editor/StyleSourceAnnotations.h"

#include <gtest/gtest.h>

#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

svg::SVGDocument ParseSvg(std::string_view source) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(source, sink);
  EXPECT_FALSE(result.hasError());
  return std::move(result).result();
}

std::string_view SourceForContribution(std::string_view source,
                                       const StyleSourceContribution& contribution) {
  return source.substr(contribution.sourceRange.start,
                       contribution.sourceRange.end - contribution.sourceRange.start);
}

std::string_view SourceForRange(std::string_view source, SourceByteRange range) {
  return source.substr(range.start, range.end - range.start);
}

const StyleSourceContribution* FindContribution(const StyleSourceAnnotations& annotations,
                                                StyleContributionKind kind,
                                                std::string_view propertyName,
                                                std::string_view sourceNeedle,
                                                std::string_view source) {
  for (const StyleSourceContribution& contribution : annotations.contributions) {
    if (contribution.kind == kind && contribution.propertyName == propertyName &&
        SourceForContribution(source, contribution).find(sourceNeedle) != std::string_view::npos) {
      return &contribution;
    }
  }

  return nullptr;
}

TEST(StyleSourceAnnotations, StylesheetRuleReportsSelectorMatchedElementChipCount) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>.hit { fill: red; }</style>
  <rect class="hit"/>
  <circle class="hit"/>
  <path/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* fill = FindContribution(
      annotations, StyleContributionKind::StylesheetDeclaration, "fill", "fill: red", kSource);
  ASSERT_NE(fill, nullptr);
  EXPECT_TRUE(fill->showChip);
  EXPECT_EQ(fill->matchedElementCount, 2);
  EXPECT_TRUE(fill->effective);
}

TEST(StyleSourceAnnotations, StylesheetChipAnchorsToSelectorRange) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .hit {
      fill: red;
      stroke: blue;
    }
  </style>
  <rect class="hit"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* fill = FindContribution(
      annotations, StyleContributionKind::StylesheetDeclaration, "fill", "fill: red", kSource);
  ASSERT_NE(fill, nullptr);
  EXPECT_TRUE(fill->showChip);
  EXPECT_EQ(SourceForRange(kSource, fill->chipRange), ".hit");

  const StyleSourceContribution* stroke = FindContribution(
      annotations, StyleContributionKind::StylesheetDeclaration, "stroke", "stroke: blue", kSource);
  ASSERT_NE(stroke, nullptr);
  EXPECT_FALSE(stroke->showChip);
  EXPECT_EQ(SourceForRange(kSource, stroke->chipRange), ".hit");
}

TEST(StyleSourceAnnotations, OverriddenStylesheetDeclarationKeepsOverrideTooltip) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>
    .hit { fill: url(#paint); }
    .hit { fill: red; }
  </style>
  <rect class="hit"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* fillUrl =
      FindContribution(annotations, StyleContributionKind::StylesheetDeclaration, "fill",
                       "fill: url(#paint)", kSource);
  ASSERT_NE(fillUrl, nullptr);
  EXPECT_FALSE(fillUrl->effective);
  EXPECT_EQ(fillUrl->matchedElementCount, 1);
  EXPECT_EQ(fillUrl->chipTooltip, "Selector matches 1 element");
  EXPECT_EQ(fillUrl->tooltip,
            "fill is overridden by a later or higher-specificity CSS declaration");
}

TEST(StyleSourceAnnotations, StylesheetChipMarksOverflowWhenSelectorMatchesTooManyElements) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>* { stroke-width: 0; }</style>
  <rect/>
  <rect/>
  <rect/>
  <rect/>
  <rect/>
  <rect/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* strokeWidth =
      FindContribution(annotations, StyleContributionKind::StylesheetDeclaration, "stroke-width",
                       "stroke-width: 0", kSource);
  ASSERT_NE(strokeWidth, nullptr);
  EXPECT_TRUE(strokeWidth->showChip);
  EXPECT_GT(strokeWidth->matchedElementCount, 5);
  EXPECT_TRUE(strokeWidth->showOverflowMarker);
  EXPECT_EQ(strokeWidth->overflowTooltip, "Too many reverse refs to draw lines");
}

TEST(StyleSourceAnnotations, ReferenceResourceElementsShowReferenceCountAndUnusedStrike) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>.hit { fill: url(#paint); }</style>
  <defs>
    <linearGradient id="paint"><stop offset="1"/></linearGradient>
    <linearGradient id="unused"><stop offset="1"/></linearGradient>
    <path id="shape"/>
  </defs>
  <use href="#shape"/>
  <rect class="hit"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* paint =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement,
                       "linearGradient", R"(id="paint")", kSource);
  ASSERT_NE(paint, nullptr);
  EXPECT_TRUE(paint->showChip);
  EXPECT_TRUE(paint->effective);
  EXPECT_EQ(paint->matchedElementCount, 1);
  EXPECT_EQ(paint->chipTooltip, "Referenced 1 time");
  EXPECT_EQ(SourceForRange(kSource, paint->chipRange), R"(<linearGradient id="paint">)");

  const StyleSourceContribution* unused =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement,
                       "linearGradient", R"(id="unused")", kSource);
  ASSERT_NE(unused, nullptr);
  EXPECT_TRUE(unused->showChip);
  EXPECT_FALSE(unused->effective);
  EXPECT_EQ(unused->matchedElementCount, 0);
  EXPECT_EQ(unused->tooltip, "linearGradient #unused is not referenced");
  EXPECT_EQ(unused->chipTooltip, "Referenced 0 times");

  const StyleSourceContribution* shape =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement, "path",
                       R"(id="shape")", kSource);
  ASSERT_NE(shape, nullptr);
  EXPECT_TRUE(shape->effective);
  EXPECT_EQ(shape->matchedElementCount, 1);
}

TEST(StyleSourceAnnotations, ReferenceResourceElementMarksOverflowWhenTooManyReverseRefs) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <defs><linearGradient id="paint"><stop offset="1"/></linearGradient></defs>
  <rect fill="url(#paint)"/>
  <rect fill="url(#paint)"/>
  <rect fill="url(#paint)"/>
  <rect fill="url(#paint)"/>
  <rect fill="url(#paint)"/>
  <rect fill="url(#paint)"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* paint =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement,
                       "linearGradient", R"(id="paint")", kSource);
  ASSERT_NE(paint, nullptr);
  EXPECT_TRUE(paint->showChip);
  EXPECT_EQ(paint->matchedElementCount, 6);
  EXPECT_TRUE(paint->showOverflowMarker);
  EXPECT_EQ(paint->overflowTooltip, "Too many reverse refs to draw lines");
}

TEST(StyleSourceAnnotations, InlineStyleDeclarationDoesNotShowSelectorChip) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <rect style="fill: red; stroke: blue"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* fill = FindContribution(
      annotations, StyleContributionKind::InlineStyleDeclaration, "fill", "fill: red", kSource);
  ASSERT_NE(fill, nullptr);
  EXPECT_FALSE(fill->showChip);
  EXPECT_EQ(fill->matchedElementCount, 1);
  EXPECT_TRUE(fill->effective);
}

TEST(StyleSourceAnnotations, PresentationAttributeDimsWhenOverriddenByInlineStyle) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <rect fill="red" style="fill: blue"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* presentationFill = FindContribution(
      annotations, StyleContributionKind::PresentationAttribute, "fill", "fill=\"red\"", kSource);
  ASSERT_NE(presentationFill, nullptr);
  EXPECT_FALSE(presentationFill->showChip);
  EXPECT_FALSE(presentationFill->effective);

  const StyleSourceContribution* inlineFill = FindContribution(
      annotations, StyleContributionKind::InlineStyleDeclaration, "fill", "fill: blue", kSource);
  ASSERT_NE(inlineFill, nullptr);
  EXPECT_TRUE(inlineFill->effective);
}

TEST(StyleSourceAnnotations, PresentationStrokeWidthDimsWhenOverriddenByStylesheet) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>.wide { stroke-width: 3; }</style>
  <rect class="wide" stroke-width="1"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* presentationStrokeWidth =
      FindContribution(annotations, StyleContributionKind::PresentationAttribute, "stroke-width",
                       "stroke-width=\"1\"", kSource);
  ASSERT_NE(presentationStrokeWidth, nullptr);
  EXPECT_FALSE(presentationStrokeWidth->effective);

  const StyleSourceContribution* stylesheetStrokeWidth =
      FindContribution(annotations, StyleContributionKind::StylesheetDeclaration, "stroke-width",
                       "stroke-width: 3", kSource);
  ASSERT_NE(stylesheetStrokeWidth, nullptr);
  EXPECT_TRUE(stylesheetStrokeWidth->showChip);
  EXPECT_EQ(stylesheetStrokeWidth->matchedElementCount, 1);
  EXPECT_TRUE(stylesheetStrokeWidth->effective);
}

TEST(StyleSourceAnnotations, PresentationAttributeStaysActiveWithoutOverride) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <rect fill="red"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* presentationFill = FindContribution(
      annotations, StyleContributionKind::PresentationAttribute, "fill", "fill=\"red\"", kSource);
  ASSERT_NE(presentationFill, nullptr);
  EXPECT_TRUE(presentationFill->effective);
}

TEST(StyleSourceAnnotations, StylesheetDeclarationStaysActiveIfItWinsForAnyMatchedElement) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>.hit { fill: red; }</style>
  <rect class="hit" style="fill: blue"/>
  <circle class="hit"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* stylesheetFill = FindContribution(
      annotations, StyleContributionKind::StylesheetDeclaration, "fill", "fill: red", kSource);
  ASSERT_NE(stylesheetFill, nullptr);
  EXPECT_EQ(stylesheetFill->matchedElementCount, 2);
  EXPECT_TRUE(stylesheetFill->effective);
}

}  // namespace
}  // namespace donner::editor
