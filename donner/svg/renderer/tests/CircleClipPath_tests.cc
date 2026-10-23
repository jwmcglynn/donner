#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

RendererBitmap RenderCircleScene(std::string_view body) {
  SVGDocument document = instantiateSubtree(
      std::string(body) + R"svg(<rect x="1" y="1" width="3" height="3" fill="lime"/>)svg", {},
      Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

void ExpectCircleReference(std::string_view clip, std::string_view circle,
                           std::string_view geometry, std::string_view label) {
  const std::string content(geometry);
  const auto actual =
      RenderCircleScene("<g clip-path='" + std::string(clip) + "'>" + content + "</g>");
  const auto expected =
      RenderCircleScene("<clipPath id='expected'>" + std::string(circle) +
                        "</clipPath><g clip-path='url(#expected)'>" + content + "</g>");
  ExpectBitmapsIdentical(actual, expected, label);
}

TEST(CircleClipPath, DefaultCircleClipsAllGroupChildren) {
  ExpectCircleReference("circle()", "<circle cx='32' cy='28' r='20'/>",
                        "<rect x='8' y='8' width='24' height='40' fill='red'/>"
                        "<rect x='32' y='8' width='24' height='40' fill='blue'/>",
                        "circle_group_default");
}

TEST(CircleClipPath, FillBoxAndPositionUseUnclippedGeometry) {
  ExpectCircleReference("circle(8px at 25% 50%) fill-box", "<circle cx='18' cy='28' r='8'/>",
                        "<rect x='8' y='12' width='40' height='32' fill='red'/>",
                        "circle_fill_box_position");
}

TEST(CircleClipPath, PercentRadiusUsesNormalizedDiagonal) {
  ExpectCircleReference("circle(25%) fill-box", "<circle cx='28' cy='28' r='9.055385138137417'/>",
                        "<rect x='8' y='12' width='40' height='32' fill='red'/>",
                        "circle_radius_diagonal");
}

TEST(CircleClipPath, DefaultAndStrokeBoxIncludeStrokeGeometry) {
  for (const std::string_view clip : {"circle()", "circle() stroke-box", "stroke-box circle()",
                                      "circle() border-box", "circle() margin-box"}) {
    SCOPED_TRACE(clip);
    ExpectCircleReference(
        clip, "<circle cx='32' cy='32' r='20'/>",
        "<rect x='16' y='16' width='32' height='32' fill='red' stroke='blue' stroke-width='8'/>",
        "circle_stroke_box");
  }
}

TEST(CircleClipPath, ExplicitFillBoxExcludesStroke) {
  for (const std::string_view box : {"fill-box", "content-box", "padding-box"}) {
    SCOPED_TRACE(box);
    ExpectCircleReference(
        "circle() " + std::string(box), "<circle cx='32' cy='32' r='16'/>",
        "<rect x='16' y='16' width='32' height='32' fill='red' stroke='blue' stroke-width='8'/>",
        "circle_fill_box_excludes_stroke");
  }
}

TEST(CircleClipPath, ViewBoxUsesNearestSvgViewport) {
  ExpectCircleReference("circle() view-box", "<circle cx='32' cy='32' r='32'/>",
                        "<rect x='8' y='8' width='48' height='48' fill='red'/>", "circle_view_box");
}

TEST(CircleClipPath, EdgeOffsetsAndFarthestSideResolveInReferenceBox) {
  ExpectCircleReference(
      "circle(6px at right 10px bottom 6px) fill-box", "<circle cx='38' cy='38' r='6'/>",
      "<rect x='8' y='12' width='40' height='32' fill='red'/>", "circle_edge_offsets");
  ExpectCircleReference(
      "circle(farthest-side at 25% 50%) fill-box", "<circle cx='18' cy='28' r='30'/>",
      "<rect x='8' y='12' width='40' height='32' fill='red'/>", "circle_farthest_side");
}

TEST(CircleClipPath, ZeroRadiusSuppressesWholeGroup) {
  const auto actual = RenderCircleScene(
      "<g clip-path='circle(0)'><rect x='8' y='8' width='40' height='40' fill='red'/></g>");
  ExpectBitmapsIdentical(actual, RenderCircleScene(""), "circle_zero_radius_group");
}

TEST(CircleClipPath, BasicShapeOnClipChildIntersectsItsGeometry) {
  const auto actual = RenderCircleScene(R"svg(
      <clipPath id="clip"><rect x="8" y="12" width="40" height="32"
          clip-path="circle(8px at 25% 50%) fill-box"/></clipPath>
      <rect width="64" height="64" fill="red" clip-path="url(#clip)"/>)svg");
  const auto expected = RenderCircleScene("<circle cx='18' cy='28' r='8' fill='red'/>");
  ExpectBitmapsIdentical(actual, expected, "circle_nested_clip_child");
}

TEST(CircleClipPath, InvalidCircleDoesNotReplaceEarlierDeclaration) {
  for (const std::string_view value : {"circle(-1px)", "circle(1px extra)", "circle(1px) nonsense",
                                       "circle(1px) fill-box stroke-box"}) {
    SCOPED_TRACE(value);
    const std::string definitions =
        "<clipPath id='clip'><circle cx='24' cy='24' r='8'/></clipPath>";
    const auto actual = RenderCircleScene(definitions +
                                          "<rect x='8' y='8' width='40' height='40' fill='red' "
                                          "style='clip-path:url(#clip);clip-path:" +
                                          std::string(value) + "'/>");
    const auto expected = RenderCircleScene("<circle cx='24' cy='24' r='8' fill='red'/>");
    ExpectBitmapsIdentical(actual, expected, "circle_invalid_declaration");
  }
}

TEST(CircleClipPath, NoneClearsAnEarlierCircleOrReference) {
  for (const std::string_view initial : {"circle(8px)", "url(#clip)"}) {
    SCOPED_TRACE(initial);
    const auto actual = RenderCircleScene(
        "<clipPath id='clip'><circle cx='24' cy='24' r='8'/></clipPath>"
        "<rect x='8' y='8' width='32' height='32' fill='red' style='clip-path:" +
        std::string(initial) + ";clip-path:none'/>");
    ExpectBitmapsIdentical(
        actual, RenderCircleScene("<rect x='8' y='8' width='32' height='32' fill='red'/>"),
        "circle_none_reset");
  }
}

TEST(CircleClipPath, ImageLeafAndGroupSupplyReferenceBoxes) {
  constexpr std::string_view kRaster =
      "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR42mP4z8DwH4QZ"
      "YAwAR8oH+Rq28akAAAAASUVORK5CYII=";
  constexpr std::string_view kSvg =
      "data:image/svg+xml,%3Csvg%20xmlns=%22http://www.w3.org/2000/svg%22%20width=%222%22%20"
      "height=%222%22%3E%3Crect%20width=%222%22%20height=%222%22%20fill=%22red%22/%3E%3C/svg%3E";
  const auto expected = RenderCircleScene("<circle cx='32' cy='32' r='16' fill='red'/>");
  for (const std::string_view uri : {kRaster, kSvg}) {
    const std::string image =
        "<image x='16' y='16' width='32' height='32' href='" + std::string(uri);
    ExpectBitmapsIdentical(RenderCircleScene(image + "' clip-path='circle()'/>"), expected,
                           "circle_image_leaf");
    ExpectBitmapsIdentical(RenderCircleScene("<g clip-path='circle()'>" + image + "'/></g>"),
                           expected, "circle_image_group");
  }
}

TEST(CircleClipPath, UseHostSuppliesShadowGeometry) {
  const auto actual = RenderCircleScene(
      "<defs><rect id='source' width='32' height='32' fill='red'/></defs>"
      "<use href='#source' x='16' y='16' clip-path='circle()'/>");
  ExpectBitmapsIdentical(actual, RenderCircleScene("<circle cx='32' cy='32' r='16' fill='red'/>"),
                         "circle_use_host");
}

TEST(CircleClipPath, StyledUsesKeepOccurrenceGeometryAndFontMetrics) {
  const auto actual = RenderCircleScene(
      "<defs><rect id='source' width='2em' height='2em' fill='red' "
      "clip-path='circle(0.5em)'/></defs>"
      "<use href='#source' x='8' y='8' font-size='8'/>"
      "<use href='#source' x='28' y='24' font-size='16'/>");
  const auto expected = RenderCircleScene(
      "<circle cx='16' cy='16' r='4' fill='red'/><circle cx='44' cy='40' r='8' fill='red'/>");
  ExpectBitmapsIdentical(actual, expected, "circle_use_occurrences");
}

TEST(CircleClipPath, TextSpanSuppliesItsOwnReferenceBox) {
  const auto actual = RenderCircleScene(
      "<text x='8' y='32' font-size='20' fill='red'>A<tspan "
      "clip-path='circle(1000px)'>B</tspan></text>");
  const auto expected = RenderCircleScene("<text x='8' y='32' font-size='20' fill='red'>AB</text>");
  ExpectBitmapsIdentical(actual, expected, "circle_text_span");
}

TEST(CircleClipPath, StrokeBoxUsesStrokeShapeInsteadOfMiterEnvelope) {
  ExpectCircleReference(
      "circle() stroke-box", "<circle cx='30' cy='30' r='12.82842712474619'/>",
      "<path d='M20 20L40 40' fill='none' stroke='red' stroke-width='8' stroke-linecap='butt'/>",
      "circle_diagonal_stroke_box");
}

TEST(CircleClipPath, NonScalingStrokeBoxUsesHostStrokeGeometry) {
  constexpr std::string_view kShape =
      "<rect x='8' y='8' width='16' height='16' fill='red' stroke='blue' stroke-width='8' "
      "vector-effect='non-scaling-stroke' transform='scale(2 1)' ";
  const auto actual = RenderCircleScene(std::string(kShape) + "clip-path='circle() stroke-box'/>");
  const auto expected =
      RenderCircleScene("<clipPath id='clip'><circle cx='16' cy='16' r='10'/></clipPath>" +
                        std::string(kShape) + "clip-path='url(#clip)'/>");
  ExpectBitmapsIdentical(actual, expected, "circle_non_scaling_stroke_box");
}

}  // namespace
}  // namespace donner::svg
