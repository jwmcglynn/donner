#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

RendererBitmap RenderInsetScene(std::string_view body) {
  SVGDocument document =
      instantiateSubtree(std::string(body) + "<rect x='1' y='1' width='3' height='3' fill='lime'/>",
                         {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

std::string ReferenceScene(std::string_view rectangle, std::string_view content) {
  return "<clipPath id='reference'>" + std::string(rectangle) +
         "</clipPath><g clip-path='url(#reference)'>" + std::string(content) + "</g>";
}

constexpr std::string_view kContent = "<rect x='8' y='12' width='40' height='32' fill='red'/>";
constexpr std::string_view kStrokeContent =
    "<rect x='8' y='12' width='40' height='32' fill='red' stroke='blue' stroke-width='8'/>";

TEST(InsetClipPath, ReferencedRectangleControl) {
  const auto actual =
      RenderInsetScene(ReferenceScene("<rect x='12' y='16' width='32' height='24'/>", kContent));
  const auto expected = RenderInsetScene("<rect x='12' y='16' width='32' height='24' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "inset_rectangle_control",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(InsetClipPath, OneToFourOffsetsExpandLikeMargins) {
  struct Example {
    std::string_view offsets;
    std::string_view rectangle;
  };
  for (const auto& example : {
           Example{"4px", "<rect x='12' y='16' width='32' height='24'/>"},
           Example{"4px 8px", "<rect x='16' y='16' width='24' height='24'/>"},
           Example{"4px 8px 12px", "<rect x='16' y='16' width='24' height='16'/>"},
           Example{"4px 8px 12px 16px", "<rect x='24' y='16' width='16' height='16'/>"},
       }) {
    SCOPED_TRACE(example.offsets);
    const auto actual = RenderInsetScene("<g clip-path='inset(" + std::string(example.offsets) +
                                         ") fill-box'>" + std::string(kContent) + "</g>");
    const auto expected = RenderInsetScene(ReferenceScene(example.rectangle, kContent));
    editor::tests::CompareBitmapToBitmap(actual, expected, "inset_expansion",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(InsetClipPath, PercentagesResolveAgainstIndependentAxes) {
  const auto actual =
      RenderInsetScene("<g clip-path='inset(25% 10%) fill-box'>" + std::string(kContent) + "</g>");
  const auto expected =
      RenderInsetScene(ReferenceScene("<rect x='12' y='20' width='32' height='16'/>", kContent));
  editor::tests::CompareBitmapToBitmap(actual, expected, "inset_percentage_axes",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(InsetClipPath, NegativeOffsetsExpandBeyondFillBox) {
  const auto actual = RenderInsetScene("<g clip-path='inset(-4px 8px -4px -4px) fill-box'>" +
                                       std::string(kStrokeContent) + "</g>");
  const auto expected = RenderInsetScene(
      ReferenceScene("<rect x='4' y='8' width='36' height='40'/>", kStrokeContent));
  editor::tests::CompareBitmapToBitmap(actual, expected, "inset_negative",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(InsetClipPath, SvgReferenceBoxesAndAliases) {
  for (const auto box : {"", "stroke-box", "border-box", "margin-box"}) {
    SCOPED_TRACE(box);
    const auto actual = RenderInsetScene("<g clip-path='inset(4px) " + std::string(box) + "'>" +
                                         std::string(kStrokeContent) + "</g>");
    const auto expected = RenderInsetScene(
        ReferenceScene("<rect x='8' y='12' width='40' height='32'/>", kStrokeContent));
    editor::tests::CompareBitmapToBitmap(actual, expected, "inset_stroke_box",
                                         editor::tests::PixelmatchIdentityParams());
  }
  for (const auto box : {"fill-box", "content-box", "padding-box"}) {
    SCOPED_TRACE(box);
    const auto actual = RenderInsetScene("<g clip-path='" + std::string(box) + " inset(4px)'>" +
                                         std::string(kStrokeContent) + "</g>");
    const auto expected = RenderInsetScene(
        ReferenceScene("<rect x='12' y='16' width='32' height='24'/>", kStrokeContent));
    editor::tests::CompareBitmapToBitmap(actual, expected, "inset_fill_box",
                                         editor::tests::PixelmatchIdentityParams());
  }
  const auto actual = RenderInsetScene(
      "<g clip-path='view-box inset(25%)'><rect width='64' height='64' fill='red'/></g>");
  const auto expected = RenderInsetScene("<rect x='16' y='16' width='32' height='32' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "inset_view_box",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(InsetClipPath, OversubscribedAndZeroAreaInsetsReplaceEarlierClip) {
  for (const auto offsets :
       {"75% 0 50% 0", "0 75% 0 50%", "50%", "0 100% 0 0", "-50% 200% 0 -50%", "1e308px"}) {
    SCOPED_TRACE(offsets);
    const auto actual = RenderInsetScene(
        "<g style='clip-path:circle(16px);clip-path:inset(" + std::string(offsets) +
        ") view-box'><rect width='64' height='64' fill='red'/></g>");
    editor::tests::CompareBitmapToBitmap(actual, RenderInsetScene(""), "inset_zero_area",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(InsetClipPath, NestedClipUsesChildReferenceGeometry) {
  const auto actual = RenderInsetScene(R"svg(
    <clipPath id="clip"><rect x="8" y="12" width="40" height="32"
      clip-path="inset(25% 10%) fill-box"/></clipPath>
    <rect width="64" height="64" fill="red" clip-path="url(#clip)"/>)svg");
  const auto expected = RenderInsetScene("<rect x='12' y='20' width='32' height='16' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "inset_nested",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(InsetClipPath, InheritedEmUsesDeclaringFontSize) {
  const auto actual = RenderInsetScene(R"svg(
    <g font-size="20" clip-path="inset(.2em) view-box">
      <rect width="64" height="64" fill="red" font-size="40" clip-path="inherit"/>
    </g>)svg");
  const auto expected = RenderInsetScene("<rect x='4' y='4' width='56' height='56' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "inset_inherited_em",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(InsetClipPath, RejectedRoundAndMalformedSyntaxKeepLastValidClip) {
  for (const auto value : {"inset()", "inset(1px 2px 3px 4px 5px)", "inset(1px,2px)",
                           "inset(2px round 1px)", "inset(1e999px)", "inset(calc(1px))"}) {
    SCOPED_TRACE(value);
    const auto actual =
        RenderInsetScene("<g style='clip-path:inset(25%) view-box;clip-path:" + std::string(value) +
                         "'><rect width='64' height='64' fill='red'/></g>");
    const auto expected =
        RenderInsetScene("<rect x='16' y='16' width='32' height='32' fill='red'/>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "inset_last_valid",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
