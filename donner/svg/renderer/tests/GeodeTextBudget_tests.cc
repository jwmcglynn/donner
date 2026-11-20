/// @file
/// Geode's frame text budget for bitmap glyphs, driven through a mock text backend whose every
/// glyph is a 1x1 bitmap strike.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/tests/MockTextBackend.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

using test::PixelAt;
using test::Rgba;
using testing::_;

constexpr double kViewportSize = 64.0;

/// Paint parameters whose fill is the solid \p rgba.
PaintParams SolidFill(const css::RGBA& rgba) {
  PaintParams paint;
  paint.fill = PaintServer::Solid{css::Color(rgba)};
  paint.fillOpacity = 1.0;
  paint.opacity = 1.0;
  return paint;
}

class GeodeTextBudgetTest : public testing::Test {
protected:
  void SetUp() override {
    auto& manager = registry_.ctx().emplace<FontManager>(registry_);
    font_ = manager.fallbackFont();
    auto backend = std::make_unique<testing::NiceMock<MockTextBackend>>();
    backend_ = backend.get();
    registry_.ctx().emplace<TextEngine>(manager, registry_, std::move(backend));
    ON_CALL(*backend_, isBitmapOnly(_)).WillByDefault(testing::Return(true));
    ON_CALL(*backend_, scaleForPixelHeight(_, _)).WillByDefault(testing::Return(1.0f));
    ON_CALL(*backend_, bitmapGlyphMetrics(_, _, _))
        .WillByDefault([this](FontHandle, int glyphIndex, float scale) {
          return std::optional<TextBackend::BitmapGlyphMetrics>({.font = font_,
                                                                 .glyphIndex = glyphIndex,
                                                                 .width = 1,
                                                                 .height = 1,
                                                                 .rgbaBytes = 4,
                                                                 .scale = 1.0,
                                                                 .requestedScale = scale});
        });
  }

  void beginFrame() {
    RenderViewport viewport;
    viewport.size = Vector2d(kViewportSize, kViewportSize);
    viewport.devicePixelRatio = 1.0;
    renderer_.beginFrame(viewport);
  }

  /// Draws two bitmap glyphs with a text budget of \p maximumBytes.
  void drawGlyphs(std::uint64_t maximumBytes) {
    renderer_.setTextMaterializationBudgetForTesting({.uniqueOutlines = 8, .bytes = maximumBytes},
                                                     8);
    auto prepared = std::make_shared<PreparedTextDraw>();
    TextRun run;
    run.font = font_;
    run.usedFontSizePx = 1.0f;
    run.glyphs = {{.glyphIndex = 1}, {.glyphIndex = 2, .xPosition = 1.0}};
    prepared->runs.push_back(std::move(run));
    prepared->elementBounds = Box2d::FromXYWH(0, 0, 2, 1);
    TextParams params;
    params.preparedTextDraw = std::move(prepared);
    renderer_.drawText(registry_, components::ComputedTextComponent(), params);
  }

  Registry registry_;
  FontHandle font_;
  testing::NiceMock<MockTextBackend>* backend_ = nullptr;
  RendererGeode renderer_;
};

// Bitmap glyph admission charges the frame text budget before any pixels are decoded, as the CPU
// renderer's does.
TEST_F(GeodeTextBudgetTest, ExhaustedBudgetRejectsBeforeBitmapDecode) {
  EXPECT_CALL(*backend_, bitmapGlyph(_, _, _)).Times(0);
  EXPECT_CALL(*backend_, bitmapGlyph(_, _, _, _, _)).Times(0);
  beginFrame();
  drawGlyphs(0);
  const RendererResourceStats stats = renderer_.resourceStats();
  renderer_.endFrame();
  EXPECT_THAT(stats.textMaterializationBudgetRejected, testing::IsTrue());
  EXPECT_THAT(stats.textUniqueOutlines, testing::Eq(0u));
}

// Text that the budget rejects partway still consumes the pattern staged for its fill, so the next
// shape paints with its own paint.
TEST_F(GeodeTextBudgetTest, RejectedTextReleasesItsStagedPattern) {
  beginFrame();
  ASSERT_THAT(renderer_.beginPatternTile(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), Transform2d()),
              testing::IsTrue());
  renderer_.setPaint(SolidFill(css::RGBA(255, 0, 0, 255)));
  renderer_.drawRect(Box2d::FromXYWH(0.0, 0.0, 8.0, 8.0), StrokeParams{});
  renderer_.endPatternTile(/*forStroke=*/false);
  drawGlyphs(0);
  ASSERT_THAT(renderer_.resourceStats().textMaterializationBudgetRejected, testing::IsTrue());
  renderer_.setPaint(SolidFill(css::RGBA(0, 0, 255, 255)));
  renderer_.drawRect(Box2d::FromXYWH(0.0, 0.0, kViewportSize, kViewportSize), StrokeParams{});
  renderer_.endFrame();
  EXPECT_THAT(PixelAt(renderer_.takeSnapshot(), 32, 32), Rgba(0, 0, 255, 255));
}

// A glyph that a stretched text path warps is admitted to the budget before its outline is
// decoded. When the budget rejects it, the pattern staged for the text's fill is still consumed,
// so the shape drawn after the text paints with its own fill.
TEST_F(GeodeTextBudgetTest, RejectedWarpedGlyphReleasesItsStagedPattern) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
        <defs>
          <pattern id="p" patternUnits="userSpaceOnUse" width="4" height="4">
            <rect width="4" height="4" fill="red"/>
          </pattern>
          <path id="curve" d="M 4 30 Q 32 4 60 30"/>
        </defs>
        <text font-size="16" fill="url(#p)"><textPath href="#curve" method="stretch">AB</textPath></text>
        <rect y="40" width="64" height="24" fill="blue"/>
      </svg>)svg",
      warnings);
  ASSERT_THAT(parsed.hasError(), testing::IsFalse()) << parsed.error();
  RendererGeode renderer;
  renderer.setTextMaterializationBudgetForTesting({.uniqueOutlines = 8, .bytes = 0}, 8);
  renderer.draw(parsed.result());
  EXPECT_THAT(renderer.resourceStats().textMaterializationBudgetRejected, testing::IsTrue());
  EXPECT_THAT(PixelAt(renderer.takeSnapshot(), 32, 52), Rgba(0, 0, 255, 255));
}

}  // namespace
}  // namespace donner::svg
