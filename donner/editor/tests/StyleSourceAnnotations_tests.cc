#include "donner/editor/StyleSourceAnnotations.h"

#include <gtest/gtest.h>

#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/svg/DocumentState.h"
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

TEST(StyleSourceAnnotations, EmptySourceReturnsNoAnnotations) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>.hit { fill: red; }</style>
  <rect class="hit"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, "");

  EXPECT_TRUE(annotations.contributions.empty());
}

TEST(StyleSourceAnnotations, ComputeAllowsConcurrentDom) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>.hit { fill: red; }</style>
  <rect class="hit"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);
  document.setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* fill = FindContribution(
      annotations, StyleContributionKind::StylesheetDeclaration, "fill", "fill: red", kSource);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->matchedElementCount, 1);
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

TEST(StyleSourceAnnotations, ReferenceResourceClassificationSkipsNonResourceDefinitionNodes) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <defs id="defs">
    <style id="style">rect { fill: url('#symbol'); }</style>
    <symbol id="symbol"><rect/></symbol>
    <linearGradient id="paint"><stop id="stop" offset="1"/></linearGradient>
  </defs>
  <use href="#symbol"/>
  <rect fill="url(#paint)"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  EXPECT_EQ(FindContribution(annotations, StyleContributionKind::ReferenceResourceElement, "defs",
                             R"(id="defs")", kSource),
            nullptr);
  EXPECT_EQ(FindContribution(annotations, StyleContributionKind::ReferenceResourceElement, "style",
                             R"(id="style")", kSource),
            nullptr);
  EXPECT_EQ(FindContribution(annotations, StyleContributionKind::ReferenceResourceElement, "stop",
                             R"(id="stop")", kSource),
            nullptr);

  const StyleSourceContribution* symbol =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement, "symbol",
                       R"(id="symbol")", kSource);
  ASSERT_NE(symbol, nullptr);
  EXPECT_TRUE(symbol->effective);
  EXPECT_EQ(symbol->matchedElementCount, 2);
}

TEST(StyleSourceAnnotations, EmptyFragmentReferencesDoNotIncrementResourceCounts) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <defs><path id="shape"/></defs>
  <use href="#"/>
  <rect fill="url(#)"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* shape =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement, "path",
                       R"(id="shape")", kSource);
  ASSERT_NE(shape, nullptr);
  EXPECT_FALSE(shape->effective);
  EXPECT_EQ(shape->matchedElementCount, 0);
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

TEST(StyleSourceAnnotations, PresentationAttributeWithoutSourceLocationIsSkippedInCascade) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <rect id="target" fill="red"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);
  document.withWriteAccess([&document](svg::DocumentWriteAccess&) {
    std::optional<svg::SVGElement> target = document.querySelector("#target");
    ASSERT_TRUE(target.has_value());
    std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(target->entityHandle());
    ASSERT_TRUE(xmlNode.has_value());
    xmlNode->clearAttributeSourceLocation("fill");
  });

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  EXPECT_EQ(FindContribution(annotations, StyleContributionKind::PresentationAttribute, "fill",
                             "fill=\"red\"", kSource),
            nullptr);
}

TEST(StyleSourceAnnotations, LaterInlineDeclarationWinsEqualSpecificityTie) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <rect style="fill: red; fill: blue"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* red = FindContribution(
      annotations, StyleContributionKind::InlineStyleDeclaration, "fill", "fill: red", kSource);
  ASSERT_NE(red, nullptr);
  EXPECT_FALSE(red->effective);

  const StyleSourceContribution* blue = FindContribution(
      annotations, StyleContributionKind::InlineStyleDeclaration, "fill", "fill: blue", kSource);
  ASSERT_NE(blue, nullptr);
  EXPECT_TRUE(blue->effective);
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

TEST(StyleSourceAnnotations, ImportantStylesheetDeclarationOverridesInlineStyle) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>rect { fill: red !important; }</style>
  <rect style="fill: blue"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* stylesheetFill =
      FindContribution(annotations, StyleContributionKind::StylesheetDeclaration, "fill",
                       "fill: red !important", kSource);
  ASSERT_NE(stylesheetFill, nullptr);
  EXPECT_TRUE(stylesheetFill->effective);

  const StyleSourceContribution* inlineFill = FindContribution(
      annotations, StyleContributionKind::InlineStyleDeclaration, "fill", "fill: blue", kSource);
  ASSERT_NE(inlineFill, nullptr);
  EXPECT_FALSE(inlineFill->effective);
  EXPECT_EQ(inlineFill->tooltip, "fill is overridden by !important CSS");
}

TEST(StyleSourceAnnotations, InvalidLocalDeclarationsRemainIneffective) {
  constexpr std::string_view kSource = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <rect fill="not-a-color" style="fill: also-not-a-color"/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* presentationFill =
      FindContribution(annotations, StyleContributionKind::PresentationAttribute, "fill",
                       "fill=\"not-a-color\"", kSource);
  ASSERT_NE(presentationFill, nullptr);
  EXPECT_FALSE(presentationFill->effective);

  const StyleSourceContribution* inlineFill =
      FindContribution(annotations, StyleContributionKind::InlineStyleDeclaration, "fill",
                       "fill: also-not-a-color", kSource);
  ASSERT_NE(inlineFill, nullptr);
  EXPECT_FALSE(inlineFill->effective);
}

TEST(StyleSourceAnnotations, LooseUrlAndHrefReferencesContributeResourceCounts) {
  constexpr std::string_view kSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <style>.quoted { fill: url( "#paint" ); stroke: url( http://example.invalid/paint ); }</style>
  <defs>
    <linearGradient id="base"><stop offset="0"/></linearGradient>
    <linearGradient id="paint" href=" #base "><stop offset="1"/></linearGradient>
    <path id="shape"/>
  </defs>
  <rect class="quoted"/>
  <use href=" #shape "/>
  <use href=" not-a-fragment "/>
</svg>)svg";
  svg::SVGDocument document = ParseSvg(kSource);

  const StyleSourceAnnotations annotations = ComputeStyleSourceAnnotations(document, kSource);

  const StyleSourceContribution* paint =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement,
                       "linearGradient", R"(id="paint")", kSource);
  ASSERT_NE(paint, nullptr);
  EXPECT_TRUE(paint->effective);
  EXPECT_EQ(paint->matchedElementCount, 1);
  EXPECT_EQ(paint->chipTooltip, "Referenced 1 time");

  const StyleSourceContribution* base =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement,
                       "linearGradient", R"(id="base")", kSource);
  ASSERT_NE(base, nullptr);
  EXPECT_TRUE(base->effective);
  EXPECT_EQ(base->matchedElementCount, 1);

  const StyleSourceContribution* shape =
      FindContribution(annotations, StyleContributionKind::ReferenceResourceElement, "path",
                       R"(id="shape")", kSource);
  ASSERT_NE(shape, nullptr);
  EXPECT_TRUE(shape->effective);
  EXPECT_EQ(shape->matchedElementCount, 1);
}

}  // namespace
}  // namespace donner::editor
