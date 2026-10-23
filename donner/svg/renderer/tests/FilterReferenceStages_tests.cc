#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

RendererBitmap RenderStageScene(std::string_view body) {
  SVGDocument document = instantiateSubtree(
      std::string(body) + R"svg(<rect x="1" y="1" width="3" height="3" fill="lime"/>)svg", {},
      Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

RendererBitmap RenderStageChain(std::string_view definitions, std::string_view filters,
                                std::string_view fill = "red") {
  return RenderStageScene(
      std::string(definitions) + R"svg(<rect x="12" y="12" width="20" height="20" fill=")svg" +
      std::string(fill) + R"svg(" filter=")svg" + std::string(filters) + R"svg("/>)svg");
}

void ExpectStageImage(const RendererBitmap& actual, std::string_view expectedBody,
                      std::string_view label) {
  ASSERT_FALSE(actual.empty());
  EXPECT_THAT(test::PixelAt(actual, 2, 2), test::RgbaEq(0, 255, 0, 255));
  ExpectBitmapsIdentical(actual, RenderStageScene(expectedBody), label);
}

TEST(FilterReferenceStages, SourceGraphicUsesPreviousFunctionOutput) {
  const auto actual =
      RenderStageChain(R"svg(<filter id="identity"><feOffset in="SourceGraphic"/></filter>)svg",
                       "brightness(0.5) url(#identity)", "#800000");
  ExpectStageImage(actual, R"svg(<rect x="12" y="12" width="20" height="20" fill="#400000"/>)svg",
                   "function_before_source_graphic");
}

TEST(FilterReferenceStages, SourceGraphicIsScopedToEachReferencedFilter) {
  const auto actual =
      RenderStageChain(R"svg(<filter id="step"><feOffset in="SourceGraphic" dx="1"/></filter>)svg",
                       "url(#step) url(#step)");
  ExpectStageImage(actual, R"svg(<rect x="14" y="12" width="20" height="20" fill="red"/>)svg",
                   "repeated_explicit_source_graphic");
}

TEST(FilterReferenceStages, SourceAlphaUsesPreviousStageAlpha) {
  const auto actual =
      RenderStageChain(R"svg(<filter id="alpha"><feOffset in="SourceAlpha"/></filter>)svg",
                       "opacity(0.5) url(#alpha)");
  ExpectStageImage(actual,
                   R"svg(<rect x="12" y="12" width="20" height="20" fill="rgba(0,0,0,0.5)"/>)svg",
                   "previous_stage_alpha");
}

TEST(FilterReferenceStages, ResultNamesDoNotLeakBetweenFilterElements) {
  const auto actual = RenderStageChain(R"svg(
      <filter id="first"><feFlood flood-color="blue" result="private"/>
        <feFlood flood-color="red"/></filter>
      <filter id="second"><feOffset in="private"/></filter>)svg",
                                       "url(#first) url(#second)");
  // An unknown name falls back to the current filter's implicit input. It cannot address a
  // result from another filter element in the CSS list.
  ExpectStageImage(actual, R"svg(<rect x="10" y="10" width="24" height="24" fill="red"/>)svg",
                   "filter_result_name_scope");
}

TEST(FilterReferenceStages, EachReferencePreservesItsFilterRegion) {
  const auto actual = RenderStageChain(R"svg(
      <filter id="wide" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64">
        <feOffset dx="10"/></filter>)svg",
                                       "url(#wide) brightness(1)");
  ExpectStageImage(actual, R"svg(<rect x="22" y="12" width="20" height="20" fill="red"/>)svg",
                   "referenced_filter_region");
}

TEST(FilterReferenceStages, EachReferencePreservesItsPrimitiveUnits) {
  const auto actual = RenderStageChain(R"svg(
      <filter id="relative" filterUnits="userSpaceOnUse" primitiveUnits="objectBoundingBox"
              x="0" y="0" width="64" height="64"><feOffset dx="0.25"/></filter>)svg",
                                       "brightness(1) url(#relative)");
  ExpectStageImage(actual, R"svg(<rect x="17" y="12" width="20" height="20" fill="red"/>)svg",
                   "referenced_primitive_units");
}

TEST(FilterReferenceStages, EmptyReferencedStageSuppressesItsGroup) {
  const auto actual = RenderStageScene(R"svg(
      <filter id="empty"/>
      <g filter="opacity(1) url(#empty)"><rect x="12" y="12" width="20" height="20" fill="red"/></g>
  )svg");
  ExpectStageImage(actual, "", "empty_filter_stage_group");
}

TEST(FilterReferenceStages, LaterFloodCanPaintAfterEmptyStage) {
  const auto actual = RenderStageChain(R"svg(
      <filter id="empty"/><filter id="flood"><feFlood flood-color="blue"/></filter>)svg",
                                       "url(#empty) url(#flood)");
  ExpectStageImage(actual, R"svg(<rect x="10" y="10" width="24" height="24" fill="blue"/>)svg",
                   "flood_after_empty_stage");
}

TEST(FilterReferenceStages, InvalidReferenceDisablesEntireChain) {
  constexpr std::string_view kExpected =
      R"svg(<rect x="12" y="12" width="20" height="20" fill="red"/>)svg";
  for (const std::string_view filters :
       {"url(#missing)", "brightness(0) url(#missing)", "url(#wrong) opacity(0)"}) {
    SCOPED_TRACE(filters);
    ExpectStageImage(RenderStageChain(R"svg(<path id="wrong" d="M0 0"/>)svg", filters), kExpected,
                     "invalid_reference_disables_chain");
  }
}

TEST(FilterReferenceStages, AggregateNodeLimitSuppressesEntireGroup) {
  std::string primitives;
  for (int i = 0; i < 17; ++i) {
    primitives += "<feOffset/>";
  }
  const auto actual = RenderStageScene("<filter id='many'>" + primitives + "</filter>" +
                                       R"svg(<g filter="url(#many) url(#many)">
        <rect x="12" y="12" width="20" height="20" fill="red"/>
      </g>)svg");
  ExpectStageImage(actual, "", "aggregate_node_limit_group");
}

TEST(FilterReferenceStages, ExpandedCapturesPreserveLeafAndGroupCoordinates) {
  constexpr std::string_view kDefinitions = R"svg(
      <filter id="step" filterUnits="userSpaceOnUse" x="-20" y="-10" width="100" height="84">
        <feOffset dx="1"/></filter>)svg";
  constexpr std::string_view kExpected =
      R"svg(<rect x="14" y="12" width="20" height="20" fill="red"/>)svg";
  ExpectStageImage(RenderStageChain(kDefinitions, "url(#step) url(#step)"), kExpected,
                   "expanded_capture_leaf");
  const auto group =
      RenderStageScene(std::string(kDefinitions) + R"svg(<g filter="url(#step) url(#step)">
        <rect x="12" y="12" width="20" height="20" fill="red"/></g>)svg");
  ExpectStageImage(group, kExpected, "expanded_capture_group");
}

TEST(FilterReferenceStages, CpuLaterStagesPreserveOriginalFillAndStrokePaint) {
  for (const bool stroke : {false, true}) {
    SCOPED_TRACE(stroke);
    const std::string paint = stroke ? "fill='none' stroke='blue' stroke-width='4'" : "fill='red'";
    const std::string shape = "<rect x='12' y='12' width='20' height='20' " + paint;
    const std::string definitions =
        "<filter id='paint' filterUnits='userSpaceOnUse' x='-20' y='-10' width='100' height='84'>"
        "<feOffset in='" +
        std::string(stroke ? "StrokePaint" : "FillPaint") + "'/></filter>";
    auto actualDocument = instantiateSubtree(
        definitions + shape + " filter='opacity(0) url(#paint)'/>", {}, Vector2i(64, 64));
    auto expectedDocument = instantiateSubtree(shape + "/>", {}, Vector2i(64, 64));
    ExpectBitmapsIdentical(RenderDocumentWithBackend(actualDocument, RendererBackend::TinySkia),
                           RenderDocumentWithBackend(expectedDocument, RendererBackend::TinySkia),
                           stroke ? "cpu_stage_stroke_paint" : "cpu_stage_fill_paint");
  }
}

TEST(FilterReferenceStages, ExpandedStagesPreserveNestedSurfaceCoordinates) {
  constexpr std::string_view kDefinitions = R"svg(
    <filter id="step" filterUnits="userSpaceOnUse" x="-20" y="-10" width="100" height="84">
      <feOffset dx="1"/></filter>
    <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="64" height="64">
      <rect width="64" height="64" fill="white"/><rect x="20" width="4" height="64"/></mask>
    <clipPath id="c"><rect x="16" y="16" width="12" height="12"/></clipPath>
    <pattern id="p" patternUnits="userSpaceOnUse" width="8" height="8">
      <rect width="8" height="8" fill="red"/><rect width="4" height="4" fill="blue"/></pattern>
  )svg";
  for (const std::string_view attributes :
       {"opacity='0.5'", "mask='url(#m)'", "clip-path='url(#c)'", "fill='url(#p)'"}) {
    SCOPED_TRACE(attributes);
    const std::string content =
        "<g " + std::string(attributes) + "><rect x='12' y='12' width='20' height='20'/></g>";
    const auto actual = RenderStageScene(std::string(kDefinitions) +
                                         "<g filter='url(#step) url(#step)'>" + content + "</g>");
    ExpectStageImage(
        actual, std::string(kDefinitions) + "<g transform='translate(2 0)'>" + content + "</g>",
        "expanded_nested_surface");
  }
}

TEST(FilterReferenceStages, StagesCanMoveOffCanvasInputsIntoView) {
  const auto actual = RenderStageScene(R"svg(
    <filter id="move" filterUnits="userSpaceOnUse" x="-64" y="0" width="192" height="64">
      <feOffset dx="20"/></filter>
    <rect x="-30" y="12" width="10" height="20" fill="red" filter="url(#move) url(#move)"/>
  )svg");
  ExpectStageImage(actual, R"svg(<rect x="10" y="12" width="10" height="20" fill="red"/>)svg",
                   "off_canvas_stage_input");
}

}  // namespace
}  // namespace donner::svg
