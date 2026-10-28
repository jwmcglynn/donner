#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextBackend.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

class DecorationBackend : public TextBackend {
public:
  FontVMetrics fontVMetrics(FontHandle) const override {
    return {.ascent = 800, .descent = -200, .lineGap = 0, .xHeight = 500, .unitsPerEm = 1000};
  }
  float scaleForPixelHeight(FontHandle, float size) const override { return size / 1000; }
  float scaleForEmToPixels(FontHandle, float size) const override { return size / 1000; }
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle) const override {
    return UnderlineMetrics{.position = -100, .thickness = 50};
  }
  std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle) const override {
    return UnderlineMetrics{.position = 300, .thickness = 40};
  }
  std::optional<SubSuperMetrics> subSuperMetrics(FontHandle) const override { return std::nullopt; }
  Path glyphOutline(FontHandle, int, float) const override { return {}; }
  bool isBitmapOnly(FontHandle) const override { return false; }
  bool isCursive(uint32_t) const override { return false; }
  bool hasSmallCapsFeature(FontHandle) const override { return false; }
  std::optional<BitmapGlyph> bitmapGlyph(FontHandle, int, float) const override {
    return std::nullopt;
  }
  ShapedRun shapeRun(FontHandle, float, std::string_view text, size_t begin, size_t length, bool,
                     FontVariant, bool) const override {
    ShapedRun run;
    for (size_t index = begin; index < std::min(text.size(), begin + length); ++index) {
      run.glyphs.push_back({.glyphIndex = 3, .xAdvance = 10, .yAdvance = 10,
                           .cluster = static_cast<uint32_t>(index)});
    }
    return run;
  }
  double crossSpanKern(FontHandle, float, FontHandle, float, uint32_t, uint32_t, bool) const override {
    return 0;
  }
};

RendererBitmap RenderDecorationScene(std::string_view body) {
  SVGDocument document = instantiateSubtree(body, {}, Vector2i(100, 100));
  auto& registry = document.registry();
  auto& fonts = registry.ctx().emplace<FontManager>(registry);
  registry.ctx().emplace<TextEngine>(fonts, registry, std::make_unique<DecorationBackend>());
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

TEST(TextDecorationThickness, ExplicitThicknessAppliesToEachLine) {
  for (const auto [line, top] : {std::pair{"underline", 38}, std::pair{"overline", 20},
                               std::pair{"line-through", 30}}) {
    SCOPED_TRACE(line);
    const auto actual = RenderDecorationScene(
        "<text x='10' y='40' font-size='20' fill='red' style='text-decoration:" +
        std::string(line) + ";text-decoration-thickness:8px'>XX</text>");
    const auto expected = RenderDecorationScene("<rect x='10' y='" + std::to_string(top) +
                                                 "' width='20' height='8' fill='red'/>");
    editor::tests::CompareBitmapToBitmap(actual, expected, line,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextDecorationThickness, ParentAndChildLinesKeepTheirOrigin) {
  const auto actual = RenderDecorationScene(R"svg(
    <text x="10" y="40" font-size="20" fill="red"
          style="text-decoration:underline;text-decoration-thickness:8px">
      <tspan font-size="40" fill="blue"
             style="text-decoration:line-through;text-decoration-thickness:2px">XX</tspan>
    </text>)svg");
  const auto expected = RenderDecorationScene(R"svg(
    <rect x="10" y="38" width="20" height="8" fill="red"/>
    <rect x="10" y="27" width="20" height="2" fill="blue"/>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_independent_origins",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, RepeatedSameLineKeepsBothOrigins) {
  const auto actual = RenderDecorationScene(R"svg(
    <text x="10" y="40" font-size="20" fill="red"
          style="text-decoration:underline;text-decoration-thickness:8px">
      <tspan fill="blue" style="text-decoration:underline;text-decoration-thickness:2px">XX</tspan>
    </text>)svg");
  const auto expected = RenderDecorationScene(R"svg(
    <rect x="10" y="38" width="20" height="8" fill="red"/>
    <rect x="10" y="41" width="20" height="2" fill="blue"/>)svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_repeated_origins",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationThickness, ChildThicknessWithoutLineDoesNotChangeParent) {
  const auto actual = RenderDecorationScene(R"svg(
    <text x="10" y="40" font-size="20" fill="red"
          style="text-decoration:underline;text-decoration-thickness:8px">
      <tspan font-size="40" fill="blue" style="text-decoration-thickness:2px">XX</tspan>
    </text>)svg");
  const auto expected = RenderDecorationScene(
      "<rect x='10' y='38' width='20' height='8' fill='red'/>");
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_unrelated_child_thickness",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
