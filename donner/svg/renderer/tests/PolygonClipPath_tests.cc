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

void ExpectPolygonReference(std::string_view clip, std::string_view path,
                            std::string_view geometry) {
  const auto actual = RenderPolygonScene("<g clip-path='" + std::string(clip) + "'>" +
                                         std::string(geometry) + "</g>");
  const auto expected = RenderPolygonScene("<clipPath id='reference'>" + std::string(path) +
                                           "</clipPath><g clip-path='url(#reference)'>" +
                                           std::string(geometry) + "</g>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "polygon_reference",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(PolygonClipPath, PercentCoordinatesUseFillBoxOriginAndEachAxis) {
  ExpectPolygonReference("polygon(0 0, 100% 0, 0 100%) fill-box", "<path d='M8 12 H48 L8 44 Z'/>",
                         "<rect x='8' y='12' width='40' height='32' fill='red'/>");
}

TEST(PolygonClipPath, ExplicitLengthsAllowCoordinatesOutsideReferenceBox) {
  ExpectPolygonReference("polygon(-4px -4px, 24px -4px, 24px 20px) fill-box",
                         "<path d='M4 8 H32 V32 Z'/>",
                         "<rect x='8' y='12' width='40' height='32' fill='red'/>");
}

TEST(PolygonClipPath, StrokeAndViewReferenceBoxes) {
  ExpectPolygonReference("polygon(0 0, 100% 0, 0 100%)", "<path d='M12 16 H44 L12 40 Z'/>",
                         "<rect x='16' y='20' width='24' height='16' fill='red' "
                         "stroke='blue' stroke-width='8'/>");
  ExpectPolygonReference("view-box polygon(25% 25%, 75% 25%, 25% 75%)",
                         "<path d='M16 16 H48 L16 48 Z'/>",
                         "<rect width='64' height='64' fill='red'/>");
}

TEST(PolygonClipPath, FillRuleControlsDoubleWoundInterior) {
  for (const auto rule : {"nonzero", "evenodd"}) {
    const std::string clip = std::string("polygon(") + rule +
                             ", 8px 8px, 56px 8px, 56px 56px, 8px 56px, 8px 8px, 56px 8px, 56px "
                             "56px, 8px 56px) view-box";
    const std::string path =
        "<path d='M8 8 H56 V56 H8 V8 H56 V56 H8 Z' clip-rule='" + std::string(rule) + "'/>";
    ExpectPolygonReference(clip, path, "<rect width='64' height='64' fill='red'/>");
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
