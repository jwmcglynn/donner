#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

RendererBitmap RenderPolygonScene(std::string_view body) {
  SVGDocument document =
      instantiateSubtree(std::string(body) + "<rect x='1' y='1' width='3' height='3' fill='lime'/>",
                         {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

TEST(PolygonClipPath, PercentCoordinatesUseFillBoxOriginAndEachAxis) {
  {
    const auto actual = RenderPolygonScene(
        "<g clip-path='" + std::string("polygon(0 0, 100% 0, 0 100%) fill-box") + "'>" +
        std::string("<rect x='8' y='12' width='40' height='32' fill='red'/>") + "</g>");
    const auto expected = RenderPolygonScene(
        "<clipPath id='reference'>" + std::string("<path d='M8 12 H48 L8 44 Z'/>") +
        "</clipPath><g clip-path='url(#reference)'>" +
        std::string("<rect x='8' y='12' width='40' height='32' fill='red'/>") + "</g>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "polygon_reference",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(PolygonClipPath, ExplicitLengthsAllowCoordinatesOutsideReferenceBox) {
  {
    const auto actual = RenderPolygonScene(
        "<g clip-path='" + std::string("polygon(-4px -4px, 24px -4px, 24px 20px) fill-box") + "'>" +
        std::string("<rect x='8' y='12' width='40' height='32' fill='red'/>") + "</g>");
    const auto expected = RenderPolygonScene(
        "<clipPath id='reference'>" + std::string("<path d='M4 8 H32 V32 Z'/>") +
        "</clipPath><g clip-path='url(#reference)'>" +
        std::string("<rect x='8' y='12' width='40' height='32' fill='red'/>") + "</g>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "polygon_reference",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(PolygonClipPath, StrokeAndViewReferenceBoxes) {
  {
    const auto actual =
        RenderPolygonScene("<g clip-path='" + std::string("polygon(0 0, 100% 0, 0 100%)") + "'>" +
                           std::string("<rect x='16' y='20' width='24' height='16' fill='red' "
                                       "stroke='blue' stroke-width='8'/>") +
                           "</g>");
    const auto expected = RenderPolygonScene(
        "<clipPath id='reference'>" + std::string("<path d='M12 16 H44 L12 40 Z'/>") +
        "</clipPath><g clip-path='url(#reference)'>" +
        std::string("<rect x='16' y='20' width='24' height='16' fill='red' "
                    "stroke='blue' stroke-width='8'/>") +
        "</g>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "polygon_reference",
                                         editor::tests::PixelmatchIdentityParams());
  }
  {
    const auto actual = RenderPolygonScene(
        "<g clip-path='" + std::string("view-box polygon(25% 25%, 75% 25%, 25% 75%)") + "'>" +
        std::string("<rect width='64' height='64' fill='red'/>") + "</g>");
    const auto expected = RenderPolygonScene(
        "<clipPath id='reference'>" + std::string("<path d='M16 16 H48 L16 48 Z'/>") +
        "</clipPath><g clip-path='url(#reference)'>" +
        std::string("<rect width='64' height='64' fill='red'/>") + "</g>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "polygon_reference",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(PolygonClipPath, FillRuleControlsDoubleWoundInterior) {
  for (const auto rule : {"nonzero", "evenodd"}) {
    const std::string clip = std::string("polygon(") + rule +
                             ", 8px 8px, 56px 8px, 56px 56px, 8px 56px, 8px 8px, 56px 8px, 56px "
                             "56px, 8px 56px) view-box";
    const std::string path =
        "<path d='M8 8 H56 V56 H8 V8 H56 V56 H8 Z' clip-rule='" + std::string(rule) + "'/>";
    {
      const auto actual =
          RenderPolygonScene("<g clip-path='" + std::string(clip) + "'>" +
                             std::string("<rect width='64' height='64' fill='red'/>") + "</g>");
      const auto expected =
          RenderPolygonScene("<clipPath id='reference'>" + std::string(path) +
                             "</clipPath><g clip-path='url(#reference)'>" +
                             std::string("<rect width='64' height='64' fill='red'/>") + "</g>");
      editor::tests::CompareBitmapToBitmap(actual, expected, "polygon_reference",
                                           editor::tests::PixelmatchIdentityParams());
    }
  }
}

TEST(PolygonClipPath, DegeneratePolygonsReplaceEarlierClipWithEmptyArea) {
  for (const auto polygon :
       {"polygon(8px 8px)", "polygon(8px 8px, 40px 40px)", "polygon(8px 8px, 24px 24px, 40px 40px)",
        "polygon(16px 16px, 16px 16px, 16px 16px)"}) {
    SCOPED_TRACE(polygon);
    const auto actual =
        RenderPolygonScene("<g style='clip-path:circle(16px);clip-path:" + std::string(polygon) +
                           "'><rect width='64' height='64' fill='red'/></g>");
    editor::tests::CompareBitmapToBitmap(actual, RenderPolygonScene(""), "polygon_empty",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(PolygonClipPath, NestedEvenOddRuleOverridesElementClipRule) {
  const auto actual = RenderPolygonScene(R"svg(
    <clipPath id="clip"><rect width="64" height="64" clip-rule="nonzero"
      clip-path="polygon(evenodd, 8px 8px, 56px 8px, 56px 56px, 8px 56px,
                         8px 8px, 56px 8px, 56px 56px, 8px 56px) view-box"/></clipPath>
    <rect width="64" height="64" fill="red" clip-path="url(#clip)"/>)svg");
  editor::tests::CompareBitmapToBitmap(actual, RenderPolygonScene(""), "polygon_nested_evenodd",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(PolygonClipPath, NestedClipChildUsesItsReferenceGeometry) {
  const auto actual = RenderPolygonScene(R"svg(
      <clipPath id="clip"><rect x="8" y="12" width="40" height="32"
          clip-path="polygon(0 0, 100% 0, 0 100%) fill-box"/></clipPath>
      <rect width="64" height="64" fill="red" clip-path="url(#clip)"/>)svg");
  const auto expected = RenderPolygonScene("<path d='M8 12 H48 L8 44 Z' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "polygon_nested",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
