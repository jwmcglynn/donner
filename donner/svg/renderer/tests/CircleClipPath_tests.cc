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

}  // namespace
}  // namespace donner::svg
