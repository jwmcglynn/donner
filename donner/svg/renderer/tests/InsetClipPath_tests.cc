#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGSVGElement.h"
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
  for (const auto offsets : {"75% 0 50% 0", "0 75% 0 50%", "50%", "0 100% 0 0", "-50% 200% 0 -50%",
                             "1e308px", "1e308em", "-1e308em"}) {
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

TEST(InsetClipPath, RootFontMutationRecomputesInheritedRem) {
  auto document = instantiateSubtree(R"svg(
    <g font-size="20" clip-path="inset(.25rem) view-box">
      <rect width="64" height="64" fill="red" font-size="40" clip-path="inherit"/>
    </g><rect x="1" y="1" width="3" height="3" fill="lime"/>)svg",
                                     {}, Vector2i(64, 64));
  for (const int fontSize : {16, 32, 16}) {
    SCOPED_TRACE(fontSize);
    const std::string fontSizeValue = std::to_string(fontSize);
    document.svgElement().setAttribute("font-size", fontSizeValue);
    ASSERT_THAT(document.svgElement().getAttribute("font-size"),
                testing::Optional(RcString(fontSizeValue)));
    const auto actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
    const int inset = fontSize / 4;
    const auto expected =
        RenderInsetScene("<rect x='" + std::to_string(inset) + "' y='" + std::to_string(inset) +
                         "' width='" + std::to_string(64 - 2 * inset) + "' height='" +
                         std::to_string(64 - 2 * inset) + "' fill='red'/>");
    editor::tests::CompareBitmapToBitmap(actual, expected, "inset_root_font_mutation",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(InsetClipPath, InheritedPercentagesKeepChildSpecificReferenceBox) {
  const auto actual = RenderInsetScene(R"svg(
    <g clip-path="inset(25%) fill-box">
      <rect width="64" height="64" fill="blue"/>
      <rect x="8" y="12" width="40" height="32" fill="red" clip-path="inherit"/>
    </g>)svg");
  const auto expected = RenderInsetScene(
      "<rect x='16' y='16' width='32' height='32' fill='blue'/>"
      "<rect x='18' y='20' width='20' height='16' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "inset_inherited_percentage",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(InsetClipPath, MalformedSyntaxKeepsLastValidClip) {
  for (const auto value : {"inset()", "inset(1px 2px 3px 4px 5px)", "inset(1px,2px)",
                           "inset(2px round)", "inset(1e999px)", "inset(calc(1px))"}) {
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

struct RoundedInsetExample {
  std::string_view arguments;
  std::string_view reference;
};

class RoundedInsetClipPathTest : public testing::TestWithParam<RoundedInsetExample> {};

TEST_P(RoundedInsetClipPathTest, MatchesIndependentSvgReference) {
  const auto& example = GetParam();
  SCOPED_TRACE(example.arguments);
  const auto actual = RenderInsetScene("<g clip-path='inset(" + std::string(example.arguments) +
                                       ") view-box'><rect width='64' height='64' fill='red'/></g>");
  const auto expected = RenderInsetScene(
      ReferenceScene(example.reference, "<rect width='64' height='64' fill='red'/>"));
  editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_reference",
                                       editor::tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(
    BorderRadii, RoundedInsetClipPathTest,
    testing::Values(
        RoundedInsetExample{"8px round 0", "<rect x='8' y='8' width='48' height='48'/>"},
        RoundedInsetExample{"8px round 8px", "<rect x='8' y='8' width='48' height='48' rx='8'/>"},
        RoundedInsetExample{"8px round 12px / 6px",
                            "<rect x='8' y='8' width='48' height='48' rx='12' ry='6'/>"},
        RoundedInsetExample{"16px 8px round 25% / 10%",
                            "<rect x='8' y='16' width='48' height='32' rx='16' ry='6.4'/>"},
        RoundedInsetExample{"8px round 40px / 20px",
                            "<rect x='8' y='8' width='48' height='48' rx='24' ry='12'/>"},
        RoundedInsetExample{"8px round 20px / 40px",
                            "<rect x='8' y='8' width='48' height='48' rx='12' ry='24'/>"},
        RoundedInsetExample{"8px round 1e308px / 5e307px",
                            "<rect x='8' y='8' width='48' height='48' rx='24' ry='12'/>"},
        RoundedInsetExample{"-4px round 16px",
                            "<rect x='-4' y='-4' width='72' height='72' rx='16'/>"},
        RoundedInsetExample{"8px round 12px / 0", "<rect x='8' y='8' width='48' height='48'/>"},
        RoundedInsetExample{"8px round 4px 8px 12px 16px / 8px 12px 16px 4px",
                            "<path d='M12 8H48C52.4182779984 8 56 13.3725830024 56 20V40"
                            "C56 48.8365559968 50.6274169976 56 44 56H24"
                            "C15.1634440032 56 8 54.2091389992 8 52V16"
                            "C8 11.5817220016 9.7908610008 8 12 8Z'/>"}));

TEST(RoundedInsetClipPath, ReferencedRoundedRectangleControl) {
  const auto actual =
      RenderInsetScene(ReferenceScene("<rect x='8' y='8' width='48' height='48' rx='8'/>",
                                      "<rect width='64' height='64' fill='red'/>"));
  const auto expected =
      RenderInsetScene("<rect x='8' y='8' width='48' height='48' rx='8' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_control",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RoundedInsetClipPath, InheritedEmUsesDeclaringFont) {
  const auto actual = RenderInsetScene(R"svg(
    <g font-size="20" clip-path="inset(8px round .4em) view-box">
      <rect width="64" height="64" fill="red" font-size="40" clip-path="inherit"/>
    </g>)svg");
  const auto expected =
      RenderInsetScene(ReferenceScene("<rect x='8' y='8' width='48' height='48' rx='8'/>",
                                      "<rect width='64' height='64' fill='red'/>"));
  editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_inherited_em",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RoundedInsetClipPath, RootFontMutationRecomputesRemRadii) {
  auto document = instantiateSubtree(R"svg(
    <g font-size="20" clip-path="inset(8px round .5rem) view-box">
      <rect width="64" height="64" fill="red" font-size="40" clip-path="inherit"/>
    </g><rect x="1" y="1" width="3" height="3" fill="lime"/>)svg",
                                     {}, Vector2i(64, 64));
  for (const int fontSize : {16, 32, 16}) {
    SCOPED_TRACE(fontSize);
    const std::string value = std::to_string(fontSize);
    document.svgElement().setAttribute("font-size", value);
    ASSERT_THAT(document.svgElement().getAttribute("font-size"),
                testing::Optional(RcString(value)));
    const auto actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
    const auto expected = RenderInsetScene(ReferenceScene(
        "<rect x='8' y='8' width='48' height='48' rx='" + std::to_string(fontSize / 2) + "'/>",
        "<rect width='64' height='64' fill='red'/>"));
    editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_root_mutation",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RoundedInsetClipPath, InheritedPercentagesUseEachReferenceBox) {
  const auto actual = RenderInsetScene(R"svg(
    <g clip-path="inset(0 round 25%) fill-box">
      <rect width="64" height="64" fill="blue"/>
      <rect x="8" y="12" width="40" height="32" fill="red" clip-path="inherit"/>
    </g>)svg");
  const auto expected = RenderInsetScene(R"svg(
    <clipPath id="parent"><rect width="64" height="64" rx="16"/></clipPath>
    <clipPath id="child"><rect x="8" y="12" width="40" height="32" rx="10" ry="8"/></clipPath>
    <g clip-path="url(#parent)"><rect width="64" height="64" fill="blue"/>
      <rect x="8" y="12" width="40" height="32" fill="red" clip-path="url(#child)"/>
    </g>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_inherited_percentage",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RoundedInsetClipPath, NestedClipPreservesTransformedChildGeometry) {
  const auto actual = RenderInsetScene(R"svg(
    <g transform="translate(4 2) scale(.75)">
      <clipPath id="nested"><rect x="8" y="8" width="48" height="48"
        clip-path="inset(0 round 8px) fill-box"/></clipPath>
      <rect width="64" height="64" fill="red" clip-path="url(#nested)"/>
    </g>)svg");
  const auto expected =
      RenderInsetScene("<g transform='translate(4 2) scale(.75)'>" +
                       ReferenceScene("<rect x='8' y='8' width='48' height='48' rx='8'/>",
                                      "<rect width='64' height='64' fill='red'/>") +
                       "</g>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_nested_transform",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RoundedInsetClipPath, StyleMutationReplacesRadii) {
  auto document = instantiateSubtree(R"svg(
    <g id="clip"><rect width="64" height="64" fill="red"/></g>
    <rect x="1" y="1" width="3" height="3" fill="lime"/>)svg",
                                     {}, Vector2i(64, 64));
  auto element = document.querySelector("#clip");
  ASSERT_THAT(element, testing::Optional(testing::_));
  for (const int radius : {8, 16, 4}) {
    SCOPED_TRACE(radius);
    const std::string value = "inset(8px round " + std::to_string(radius) + "px) view-box";
    element->setAttribute("clip-path", value);
    ASSERT_THAT(element->getAttribute("clip-path"), testing::Optional(RcString(value)));
    const auto actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
    const auto expected = RenderInsetScene(ReferenceScene(
        "<rect x='8' y='8' width='48' height='48' rx='" + std::to_string(radius) + "'/>",
        "<rect width='64' height='64' fill='red'/>"));
    editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_style_mutation",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RoundedInsetClipPath, InvalidRadiusKeepsLastValidDeclaration) {
  for (const auto value : {"-1px", "1px /", "/ 1px", "1px / 2px / 3px", "1px 2px 3px 4px 5px",
                           "1e999px", "1ex", "1vw", "calc(1px)"}) {
    SCOPED_TRACE(value);
    const auto actual = RenderInsetScene(
        "<g style='clip-path:inset(8px round 8px) view-box;clip-path:inset(0 round " +
        std::string(value) + ") view-box'><rect width='64' height='64' fill='red'/></g>");
    const auto expected =
        RenderInsetScene(ReferenceScene("<rect x='8' y='8' width='48' height='48' rx='8'/>",
                                        "<rect width='64' height='64' fill='red'/>"));
    editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_invalid_radius",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RoundedInsetClipPath, PercentageRadiiUseSelectedFillAndStrokeBoxes) {
  struct Example {
    std::string_view box;
    std::string_view reference;
  };
  for (const auto& example : {
           Example{"fill-box", "<rect x='12' y='16' width='32' height='24' rx='10' ry='8'/>"},
           Example{"stroke-box", "<rect x='8' y='12' width='40' height='32' rx='12' ry='10'/>"},
           Example{"", "<rect x='8' y='12' width='40' height='32' rx='12' ry='10'/>"},
       }) {
    SCOPED_TRACE(example.box);
    const auto actual =
        RenderInsetScene("<g clip-path='inset(4px round 25%) " + std::string(example.box) + "'>" +
                         std::string(kStrokeContent) + "</g>");
    const auto expected = RenderInsetScene(ReferenceScene(example.reference, kStrokeContent));
    editor::tests::CompareBitmapToBitmap(actual, expected, "rounded_inset_selected_box",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(RoundedInsetClipPath, OversubscribedOffsetsAndComputedOverflowRemainEmpty) {
  for (const auto value :
       {"50% round 8px", "75% 0 50% round 8px", "0 round 1e308em", "0 round 1e308rem"}) {
    SCOPED_TRACE(value);
    const auto actual =
        RenderInsetScene("<g clip-path='inset(" + std::string(value) +
                         ") view-box'><rect width='64' height='64' fill='red'/></g>");
    editor::tests::CompareBitmapToBitmap(actual, RenderInsetScene(""), "rounded_inset_empty",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
