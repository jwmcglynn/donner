#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
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
  editor::tests::CompareBitmapToBitmap(actual, expected, label,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(EllipseClipPath, DefaultRadiiResolveIndependently) {
  ExpectCircleReference("ellipse()", "<ellipse cx='28' cy='28' rx='20' ry='16'/>",
                        "<rect x='8' y='12' width='40' height='32' fill='red'/>",
                        "ellipse_defaults");
}

TEST(EllipseClipPath, PercentRadiiUseTheirOwnAxis) {
  ExpectCircleReference("ellipse(25% 25%) fill-box", "<ellipse cx='28' cy='28' rx='10' ry='8'/>",
                        "<rect x='8' y='12' width='40' height='32' fill='red'/>",
                        "ellipse_percent_axes");
}

TEST(EllipseClipPath, RadiusKeywordsAndEdgeOffsets) {
  ExpectCircleReference("ellipse(closest-side farthest-side at right 10px bottom 6px) fill-box",
                        "<ellipse cx='38' cy='38' rx='10' ry='26'/>",
                        "<rect x='8' y='12' width='40' height='32' fill='red'/>",
                        "ellipse_keywords_offsets");
}

TEST(EllipseClipPath, StrokeAndViewReferenceBoxes) {
  ExpectCircleReference("ellipse() stroke-box", "<ellipse cx='28' cy='28' rx='24' ry='20'/>",
                        "<rect x='8' y='12' width='40' height='32' fill='red' "
                        "stroke='blue' stroke-width='8'/>",
                        "ellipse_stroke_box");
  ExpectCircleReference("view-box ellipse(25% 12.5% at 50% 50%)",
                        "<ellipse cx='32' cy='32' rx='16' ry='8'/>",
                        "<rect width='64' height='64' fill='red'/>", "ellipse_view_box");
}

TEST(EllipseClipPath, ZeroRadiusSuppressesWholeGroup) {
  for (const auto clip : {"ellipse(0 8px)", "ellipse(8px 0)"}) {
    const auto actual = RenderCircleScene("<g clip-path='" + std::string(clip) +
                                          "'><rect width='64' height='64' fill='red'/></g>");
    editor::tests::CompareBitmapToBitmap(actual, RenderCircleScene(""), "ellipse_zero_radius",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(EllipseClipPath, NestedClipChildIntersectsItsGeometry) {
  const auto actual = RenderCircleScene(R"svg(
      <clipPath id="clip"><rect x="8" y="12" width="40" height="32"
          clip-path="ellipse(8px 4px at 25% 50%) fill-box"/></clipPath>
      <rect width="64" height="64" fill="red" clip-path="url(#clip)"/>)svg");
  const auto expected = RenderCircleScene("<ellipse cx='18' cy='28' rx='8' ry='4' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "ellipse_nested_clip_child",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(EllipseClipPath, TransformedNestedGroupAndStylesheet) {
  const auto actual = RenderCircleScene(R"svg(
      <style>.clip { clip-path: ellipse(12px 8px at 24px 28px) view-box; }</style>
      <g transform="translate(8 4)" class="clip">
        <g clip-path="circle(12px at 28px 28px) view-box">
          <rect width="64" height="64" fill="red"/>
        </g>
      </g>)svg");
  const auto expected = RenderCircleScene(R"svg(
      <clipPath id="outer"><ellipse cx="24" cy="28" rx="12" ry="8"/></clipPath>
      <clipPath id="inner"><circle cx="28" cy="28" r="12"/></clipPath>
      <g transform="translate(8 4)" clip-path="url(#outer)">
        <g clip-path="url(#inner)"><rect width="64" height="64" fill="red"/></g>
      </g>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "ellipse_transformed_nested_group",
                                       editor::tests::PixelmatchIdentityParams());
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
  editor::tests::CompareBitmapToBitmap(actual, RenderCircleScene(""), "circle_zero_radius_group",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(CircleClipPath, BasicShapeOnClipChildIntersectsItsGeometry) {
  const auto actual = RenderCircleScene(R"svg(
      <clipPath id="clip"><rect x="8" y="12" width="40" height="32"
          clip-path="circle(8px at 25% 50%) fill-box"/></clipPath>
      <rect width="64" height="64" fill="red" clip-path="url(#clip)"/>)svg");
  const auto expected = RenderCircleScene("<circle cx='18' cy='28' r='8' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "circle_nested_clip_child",
                                       editor::tests::PixelmatchIdentityParams());
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
    editor::tests::CompareBitmapToBitmap(actual, expected, "circle_invalid_declaration",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(CircleClipPath, NoneClearsAnEarlierCircleOrReference) {
  for (const std::string_view initial : {"circle(8px)", "url(#clip)"}) {
    SCOPED_TRACE(initial);
    const auto actual = RenderCircleScene(
        "<clipPath id='clip'><circle cx='24' cy='24' r='8'/></clipPath>"
        "<rect x='8' y='8' width='32' height='32' fill='red' style='clip-path:" +
        std::string(initial) + ";clip-path:none'/>");
    editor::tests::CompareBitmapToBitmap(
        actual, RenderCircleScene("<rect x='8' y='8' width='32' height='32' fill='red'/>"),
        "circle_none_reset", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(CircleClipPath, StyleMutationRebuildsAndClearsCircleGeometry) {
  constexpr std::string_view kContent =
      "<rect id='target' x='8' y='8' width='48' height='48' fill='red'/>"
      "<rect x='1' y='1' width='3' height='3' fill='lime'/>";
  SVGDocument document = instantiateSubtree(kContent, {}, Vector2i(64, 64));
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  for (const std::string_view clip :
       {"circle(8px at 16px 16px) view-box", "none", "circle(12px at 44px 40px) view-box"}) {
    target->setAttribute("style", "clip-path:" + std::string(clip));
    const auto actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
    const auto expected =
        RenderCircleScene("<rect x='8' y='8' width='48' height='48' fill='red' style='clip-path:" +
                          std::string(clip) + "'/>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "circle_style_mutation",
                                         editor::tests::PixelmatchIdentityParams());
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
    editor::tests::CompareBitmapToBitmap(RenderCircleScene(image + "' clip-path='circle()'/>"),
                                         expected, "circle_image_leaf",
                                         editor::tests::PixelmatchIdentityParams());
    editor::tests::CompareBitmapToBitmap(
        RenderCircleScene("<g clip-path='circle()'>" + image + "'/></g>"), expected,
        "circle_image_group", editor::tests::PixelmatchIdentityParams());
  }
}

TEST(CircleClipPath, ImageUseSuppliesOccurrenceReferenceBox) {
  constexpr std::string_view kUri =
      "data:image/svg+xml,%3Csvg%20xmlns=%22http://www.w3.org/2000/svg%22%20width=%222%22%20"
      "height=%222%22%3E%3Crect%20width=%222%22%20height=%222%22%20fill=%22red%22/%3E%3C/svg%3E";
  const auto actual = RenderCircleScene(
      "<defs><image id='image' width='32' height='32' href='" + std::string(kUri) +
      "'/></defs><use href='#image' x='16' y='16' clip-path='circle()'/>");
  editor::tests::CompareBitmapToBitmap(
      actual, RenderCircleScene("<circle cx='32' cy='32' r='16' fill='red'/>"), "circle_image_use",
      editor::tests::PixelmatchIdentityParams());
}

TEST(CircleClipPath, HiddenTextRetainsStrokeReferenceGeometry) {
  const std::string prefix =
      "<g clip-path='circle() stroke-box'>"
      "<rect x='8' y='8' width='48' height='48' fill='red'/>"
      "<text x='8' y='40' font-size='40' stroke='blue' "
      "stroke-width='24' ";
  const auto hidden = RenderCircleScene(prefix + "visibility='hidden'>W</text></g>");
  const auto transparent = RenderCircleScene(prefix + "opacity='0'>W</text></g>");
  editor::tests::CompareBitmapToBitmap(hidden, transparent, "circle_hidden_text_stroke_box",
                                       editor::tests::PixelmatchIdentityParams());
}

#ifdef DONNER_TEXT_ENABLED
TEST(CircleClipPath, CurvedStretchWorkRefusalSuppressesWholeCircleClip) {
  SVGDocument document = instantiateSubtree(
      R"svg(<defs><path id="curve" d="M8 44 C8 4 56 4 56 44"/></defs>
        <g clip-path="circle() stroke-box">
          <rect x="4" y="4" width="56" height="56" fill="red"/>
          <text visibility="hidden" font-family="Public Sans" font-size="28" stroke="blue">
            <textPath href="#curve" method="stretch">Curve</textPath>
          </text>
        </g>
        <rect x="1" y="1" width="3" height="3" fill="lime"/>)svg",
      {}, Vector2i(64, 64));
  components::DocumentResourceFamilyBudget::Limits limits;
  limits.textPathMeasurementWorkUnits = 2;
  auto family = std::make_shared<components::DocumentResourceFamilyBudget>(limits);
  document.registry().ctx().get<components::DocumentResourceFamilyContext>().budget = family;

  const auto actual = RenderDocumentWithBackend(document, ActiveRendererBackend());

  editor::tests::CompareBitmapToBitmap(actual, RenderCircleScene(""),
                                       "circle_curved_stretch_work_refusal",
                                       editor::tests::PixelmatchIdentityParams());
  EXPECT_THAT(family->securityStats().textPathMeasurementWorkRejected, testing::IsTrue());
}
#endif

TEST(CircleClipPath, UseHostSuppliesShadowGeometry) {
  const auto actual = RenderCircleScene(
      "<defs><rect id='source' width='32' height='32' fill='red'/></defs>"
      "<use href='#source' x='16' y='16' clip-path='circle()'/>");
  editor::tests::CompareBitmapToBitmap(
      actual, RenderCircleScene("<circle cx='32' cy='32' r='16' fill='red'/>"), "circle_use_host",
      editor::tests::PixelmatchIdentityParams());
}

TEST(CircleClipPath, StyledUsesKeepOccurrenceGeometryAndFontMetrics) {
  const auto actual = RenderCircleScene(
      "<defs><rect id='source' width='2em' height='2em' fill='red' "
      "clip-path='circle(0.5em)'/></defs>"
      "<use href='#source' x='8' y='8' font-size='8'/>"
      "<use href='#source' x='28' y='24' font-size='16'/>");
  const auto expected = RenderCircleScene(
      "<circle cx='16' cy='16' r='4' fill='red'/><circle cx='44' cy='40' r='8' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "circle_use_occurrences",
                                       editor::tests::PixelmatchIdentityParams());
}

#ifdef DONNER_TEXT_ENABLED
void ExpectVisibleRedText(const RendererBitmap& bitmap) {
  EXPECT_GT(test::CountPixelsWhere(
                bitmap, [](const auto& pixel) { return pixel[0] > pixel[1] && pixel[3] > 0; }),
            0u);
}

TEST(CircleClipPath, TextUseLargeCircleKeepsVisibleText) {
  constexpr std::string_view kDefinitions = "<defs><text id='text' y='30'>Hello</text></defs>";
  const auto expected = RenderCircleScene(std::string(kDefinitions) +
                                          "<use href='#text' x='4' font-size='18' fill='red'/>");
  ExpectVisibleRedText(expected);
  for (const std::string_view box : {"fill-box", "stroke-box"}) {
    SCOPED_TRACE(box);
    const std::string clip = "circle(1000px) " + std::string(box);
    const auto actual = RenderCircleScene(std::string(kDefinitions) +
                                          "<use href='#text' x='4' font-size='18' fill='red' "
                                          "clip-path='" +
                                          clip + "'/>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "circle_text_use_large",
                                         editor::tests::PixelmatchIdentityParams());
    const auto group =
        RenderCircleScene(std::string(kDefinitions) + "<g clip-path='" + clip +
                          "'><use href='#text' x='4' font-size='18' fill='red'/></g>");
    editor::tests::CompareBitmapToBitmap(group, expected, "circle_text_use_group_large",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(CircleClipPath, TextUsesKeepStyledReferenceBoxes) {
  constexpr std::string_view kSpans = "<tspan font-size='125%' stroke-width='0.15em'>A</tspan>";
  for (const std::string_view box : {"fill-box", "stroke-box"}) {
    SCOPED_TRACE(box);
    const std::string clip = "circle(8px at 25% 50%) " + std::string(box);
    const std::string actualContent =
        "<defs><text id='text'>" + std::string(kSpans) +
        "</text></defs>"
        "<use href='#text' x='4' y='24' font-size='16' fill='red' stroke='red' "
        "clip-path='" +
        clip +
        "'/>"
        "<use href='#text' x='28' y='48' font-size='28' fill='red' stroke='red' "
        "clip-path='" +
        clip + "'/>";
    const std::string expectedContent =
        "<g transform='translate(4 24)' font-size='16' fill='red' stroke='red' "
        "clip-path='" +
        clip + "'><text>" + std::string(kSpans) +
        "</text></g>"
        "<g transform='translate(28 48)' font-size='28' fill='red' stroke='red' "
        "clip-path='" +
        clip + "'><text>" + std::string(kSpans) + "</text></g>";
    const auto expected = RenderCircleScene(expectedContent);
    ExpectVisibleRedText(expected);
    editor::tests::CompareBitmapToBitmap(RenderCircleScene(actualContent), expected,
                                         "circle_text_use_styled_boxes",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(CircleClipPath, GroupCircleIncludesTransformedTextUseStroke) {
  constexpr std::string_view kText = "<text y='30'><tspan>A</tspan></text>";
  const auto actual = RenderCircleScene(
      "<defs><g id='source'>" + std::string(kText) +
      "</g></defs>"
      "<g clip-path='circle(12px at 30% 50%) stroke-box'>"
      "<use href='#source' transform='translate(6 4) scale(1.2 .8)' font-size='32' "
      "fill='red' stroke='red' stroke-width='4'/></g>");
  const auto expected = RenderCircleScene(
      "<g clip-path='circle(12px at 30% 50%) stroke-box'>"
      "<g transform='translate(6 4) scale(1.2 .8)' font-size='32' fill='red' "
      "stroke='red' stroke-width='4'>" +
      std::string(kText) + "</g></g>");
  ExpectVisibleRedText(expected);
  editor::tests::CompareBitmapToBitmap(actual, expected, "circle_group_shadow_text_stroke",
                                       editor::tests::PixelmatchIdentityParams());
}
#endif

TEST(CircleClipPath, TextSpanSuppliesItsOwnReferenceBox) {
  const auto actual = RenderCircleScene(
      "<text x='8' y='32' font-size='20' fill='red'>A<tspan "
      "clip-path='circle(1000px)'>B</tspan></text>");
  const auto expected = RenderCircleScene("<text x='8' y='32' font-size='20' fill='red'>AB</text>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "circle_text_span",
                                       editor::tests::PixelmatchIdentityParams());
}

#ifdef DONNER_TEXT_ENABLED
TEST(CircleClipPath, TransformedTextSpanKeepsTextRootReferenceCoordinates) {
  const auto circleFor = [](std::string_view transform) {
    SVGDocument document = instantiateSubtree(
        "<text x='8' y='28' transform='translate(3 4)' font-size='20'>"
        "<tspan id='span' transform='" +
            std::string(transform) + "'>A</tspan></text>",
        {}, Vector2i(64, 64));
    (void)RenderDocumentWithBackend(document, ActiveRendererBackend());
    const auto span = document.querySelector("#span");
    EXPECT_THAT(span, testing::Optional(testing::_));
    CircleClipPath circle;
    circle.box = CircleClipPath::Box::Fill;
    circle.radiusType = CircleClipPath::Radius::Length;
    circle.radius = Lengthd(6.0, Lengthd::Unit::None);
    components::CircleClipGeometryBudget budget;
    return components::ShapeSystem().circleClipPath(span->entityHandle(), circle, budget);
  };

  const Path ordinary = circleFor("translate(0 0)");
  const Path transformed = circleFor("translate(6 2)");
  ASSERT_THAT(ordinary.commands(), testing::Not(testing::IsEmpty()));
  ASSERT_THAT(transformed.commands(), testing::Not(testing::IsEmpty()));
  EXPECT_THAT(transformed.bounds(), BoxEq(ordinary.bounds().topLeft - Vector2d(6, 2),
                                          ordinary.bounds().bottomRight - Vector2d(6, 2)));
}
#endif

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
  editor::tests::CompareBitmapToBitmap(actual, expected, "circle_non_scaling_stroke_box",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
