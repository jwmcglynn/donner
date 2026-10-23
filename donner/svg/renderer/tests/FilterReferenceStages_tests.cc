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

}  // namespace
}  // namespace donner::svg
