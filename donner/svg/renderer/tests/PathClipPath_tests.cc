#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

RendererBitmap RenderPathScene(std::string_view body) {
  auto document =
      instantiateSubtree(std::string(body) + "<rect x='1' y='1' width='3' height='3' fill='lime'/>",
                         {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

std::string PathReference(std::string_view data, std::string_view content,
                          std::string_view rule = "nonzero") {
  return "<clipPath id='reference'><path d='" + std::string(data) + "' clip-rule='" +
         std::string(rule) + "'/></clipPath><g clip-path='url(#reference)'>" +
         std::string(content) + "</g>";
}

constexpr std::string_view kContent = "<rect width='64' height='64' fill='red'/>";

TEST(PathClipPath, ReferencedPathControl) {
  const auto actual = RenderPathScene(PathReference("M8 12H48V44H8Z", kContent));
  const auto expected = RenderPathScene("<rect x='8' y='12' width='40' height='32' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "path_control",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(PathClipPath, CurvesArcsRelativeCommandsAndImplicitClosure) {
  for (const auto data :
       {"M8 12H48V44H8", "m8 12h40v32h-40", "M8 32Q32 0 56 32T8 32",
        "M8 32C8 8 32 8 32 32S56 56 56 32", "M8 32A24 20 0 1 1 56 32A24 20 0 1 1 8 32"}) {
    SCOPED_TRACE(data);
    const auto actual = RenderPathScene("<g style='clip-path:path(\"" + std::string(data) +
                                        "\") view-box'>" + std::string(kContent) + "</g>");
    const auto expected = RenderPathScene(PathReference(data, kContent));
    editor::tests::CompareBitmapToBitmap(actual, expected, "path_commands",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(PathClipPath, FillRuleIsIndependentOfElementAndAncestorClipRule) {
  constexpr std::string_view data = "M8 8H56V56H8Z M20 20H44V44H20Z";
  for (const auto rule : {"evenodd", "nonzero", ""}) {
    SCOPED_TRACE(rule);
    const std::string prefix = std::string(rule).empty() ? "" : std::string(rule) + ",";
    const auto actual = RenderPathScene(
        "<g clip-rule='evenodd' fill-rule='evenodd' style='clip-path:path(" + prefix + "\"" +
        std::string(data) + "\") view-box'>" + std::string(kContent) + "</g>");
    const auto expected = RenderPathScene(
        PathReference(data, kContent, std::string(rule).empty() ? "nonzero" : rule));
    editor::tests::CompareBitmapToBitmap(actual, expected, "path_winding",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(PathClipPath, OpenContoursCloseIndependentlyAndAllowNegativeCoordinates) {
  const auto actual = RenderPathScene(R"svg(<g style='clip-path:path(evenodd,
      "M-4 8H56V56H-4 M20 20H44V44H20") view-box'>
      <rect width="64" height="64" fill="red"/></g>)svg");
  const auto expected =
      RenderPathScene(PathReference("M-4 8H56V56H-4Z M20 20H44V44H20Z", kContent, "evenodd"));
  editor::tests::CompareBitmapToBitmap(actual, expected, "path_open_contours",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(PathClipPath, ReferenceBoxesTranslateWithoutScalingPathCoordinates) {
  constexpr std::string_view content =
      "<rect x='8' y='12' width='40' height='32' fill='red' stroke='blue' stroke-width='8'/>";
  for (const auto box : {"fill-box", "content-box", "padding-box", "stroke-box", "border-box",
                         "margin-box", "", "view-box"}) {
    SCOPED_TRACE(box);
    const bool fill = std::string_view(box) == "fill-box" ||
                      std::string_view(box) == "content-box" ||
                      std::string_view(box) == "padding-box";
    const bool view = std::string_view(box) == "view-box";
    const auto actual =
        RenderPathScene("<g style='clip-path:" + std::string(box) + " path(\"M4 4h24v20h-24z\")'>" +
                        std::string(content) + "</g>");
    const auto expected = RenderPathScene(PathReference(fill   ? "M12 16h24v20h-24z"
                                                        : view ? "M4 4h24v20h-24z"
                                                               : "M8 12h24v20h-24z",
                                                        content));
    editor::tests::CompareBitmapToBitmap(actual, expected, "path_reference_box",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(PathClipPath, NestedReferenceAndTransformedPathKeepCssWinding) {
  const auto actual = RenderPathScene(R"svg(
    <clipPath id="clip" clip-rule="nonzero">
      <rect width="64" height="64" clip-rule="nonzero" transform="translate(4 8) scale(.75)"
        style='clip-path:path(evenodd,"M0 0H64V64H0Z M16 16H48V48H16Z") view-box'/>
    </clipPath><g clip-path="inset(4px) view-box">
      <rect width="64" height="64" fill="red" clip-path="url(#clip)"/>
    </g>)svg");
  const auto expected = RenderPathScene(R"svg(
    <clipPath id="outer"><rect x="4" y="4" width="56" height="56"/></clipPath>
    <clipPath id="inner"><path transform="translate(4 8) scale(.75)" clip-rule="evenodd"
       d="M0 0H64V64H0Z M16 16H48V48H16Z"/></clipPath>
    <g clip-path="url(#outer)"><rect width="64" height="64" fill="red" clip-path="url(#inner)"/></g>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "path_nested_transform",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(PathClipPath, InvalidDataPreservesEarlierDeclarationAndValidDegeneratesClipEmpty) {
  for (const auto data : {"", " ", "L8 8", "M8 8L", "M8 8L20 20 garbage", "M0 0A1 1 0 2 0 10 10",
                          "M0px 0", "M0 0L50% 50%"}) {
    SCOPED_TRACE(data);
    const auto actual =
        RenderPathScene("<g style='clip-path:path(\"M8 12H48V44H8Z\") view-box;clip-path:path(\"" +
                        std::string(data) + "\")'>" + std::string(kContent) + "</g>");
    const auto expected = RenderPathScene(PathReference("M8 12H48V44H8Z", kContent));
    editor::tests::CompareBitmapToBitmap(actual, expected, "path_invalid_fallback",
                                         editor::tests::PixelmatchIdentityParams());
  }
  for (const auto data : {"M8 8", "M8 8L8 8", "M0 0L1e308 0L0 1e308Z"}) {
    SCOPED_TRACE(data);
    const auto actual =
        RenderPathScene("<g style='clip-path:inset(4px) view-box;clip-path:path(\"" +
                        std::string(data) + "\") view-box'>" + std::string(kContent) + "</g>");
    editor::tests::CompareBitmapToBitmap(actual, RenderPathScene(""), "path_degenerate",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
