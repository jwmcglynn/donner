#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"

namespace donner::svg::compositor {
namespace {

TEST(CompositorGeodeSplashTest, PublishesTilesForFullArtworkOnColdFrame) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());

  Renderer renderer;
  ASSERT_TRUE(renderer.requiresTextureSnapshotPresentation());
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport viewport;
  viewport.size = Vector2d(1536, 1024);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);
  const auto tiles = compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  EXPECT_THAT(tiles, ::testing::Not(::testing::IsEmpty()))
      << "allocation failures=" << compositor.lastRenderFrameStats().textureAllocationFailureCount;
  EXPECT_THAT(tiles, ::testing::Contains(::testing::Truly([](const CompositorTile& tile) {
                return tile.textureSnapshot != nullptr || !tile.bitmap.empty();
              })));
}

TEST(CompositorGeodeSplashTest, AuxiliaryTextPreviewDoesNotDepleteSplashTiles) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());

  Renderer renderer;
  auto preview = renderer.createOffscreenInstance();
  ASSERT_NE(preview, nullptr);
  auto previewParsed = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 196 24"><text x="1" y="18" font-size="16">Preview</text></svg>)",
      warnings);
  ASSERT_FALSE(previewParsed.hasError());
  SVGDocument previewDocument = std::move(previewParsed.result());
  previewDocument.setCanvasSize(196, 24);
  RenderViewport previewViewport;
  previewViewport.size = Vector2d(196, 24);
  previewViewport.devicePixelRatio = 1.0;
  renderer.beginFrameResourceScope();
  RendererDriver(*preview).draw(previewDocument, previewViewport, Transform2d());
  ASSERT_FALSE(preview->takeSnapshot().empty());
  renderer.endFrameResourceScope();

  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport viewport;
  viewport.size = Vector2d(1536, 1024);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);
  const auto tiles = compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  EXPECT_THAT(tiles, ::testing::Not(::testing::IsEmpty()))
      << "allocation failures=" << compositor.lastRenderFrameStats().textureAllocationFailureCount;
}

TEST(CompositorGeodeSplashTest, RepeatedAuxiliaryTextPreviewsPreserveSplashTiles) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());

  Renderer renderer;
  auto preview = renderer.createOffscreenInstance();
  ASSERT_NE(preview, nullptr);
  RenderViewport previewViewport;
  previewViewport.size = Vector2d(196, 24);
  previewViewport.devicePixelRatio = 1.0;
  for (int index = 0; index < 32; ++index) {
    auto previewParsed = parser::SVGParser::ParseSVG(
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 196 24"><text x="1" y="18">Preview</text></svg>)",
        warnings);
    ASSERT_FALSE(previewParsed.hasError());
    SVGDocument previewDocument = std::move(previewParsed.result());
    previewDocument.setCanvasSize(196, 24);
    renderer.beginFrameResourceScope();
    RendererDriver(*preview).draw(previewDocument, previewViewport, Transform2d());
    const RendererBitmap bitmap = preview->takeSnapshot();
    renderer.endFrameResourceScope();
    ASSERT_FALSE(bitmap.empty()) << "auxiliary preview " << index;
  }

  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport viewport;
  viewport.size = Vector2d(1536, 1024);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);
  EXPECT_THAT(compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All),
              ::testing::Not(::testing::IsEmpty()))
      << "allocation failures=" << compositor.lastRenderFrameStats().textureAllocationFailureCount;
}

TEST(CompositorGeodeSplashTest, RetinaScalePublishesTilesWithinSurfaceBudget) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(3072, 2048);

  Renderer renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport viewport;
  viewport.size = Vector2d(3072, 2048);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);
  const auto tiles = compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  EXPECT_THAT(tiles, ::testing::Not(::testing::IsEmpty()))
      << "allocation failures=" << compositor.lastRenderFrameStats().textureAllocationFailureCount;
}

TEST(CompositorGeodeSplashTest, RetinaViewportCropPublishesTiles) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(3072, 2048);

  Renderer renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport viewport;
  viewport.size = Vector2d(2056, 1456);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport, Transform2d::Translate(-508.0, -296.0));
  const auto tiles = compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  EXPECT_THAT(tiles, ::testing::Not(::testing::IsEmpty()))
      << "allocation failures=" << compositor.lastRenderFrameStats().textureAllocationFailureCount;
}

}  // namespace
}  // namespace donner::svg::compositor
