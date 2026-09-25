#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererGeode.h"

namespace donner::svg::compositor {
namespace {

TEST(CompositorGeodeSplashTest, RetinaSelectionKeepsCompleteVisibleTilesForFirstDrag) {
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
  compositor.setSkipMainComposeDuringSplit(true);
  RenderViewport visibleViewport;
  visibleViewport.size = Vector2d(2056, 1456);
  visibleViewport.devicePixelRatio = 1.0;
  const Transform2d visibleSurface = Transform2d::Translate(Vector2d(-508, -296));
  compositor.renderFrame(visibleViewport, visibleSurface);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());

  auto letter = document.querySelector("#letter-G");
  ASSERT_TRUE(letter.has_value());
  const Entity letterEntity = letter->unsafeEntityHandle().entity();
  ASSERT_TRUE(compositor.promoteEntity(letterEntity, InteractionHint::Selection));
  compositor.renderFrame(visibleViewport, visibleSurface);
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_EQ(compositor.lastRenderFrameStats().textureAllocationFailureCount, 0);

  ASSERT_TRUE(compositor.promoteEntity(letterEntity, InteractionHint::ActiveDrag));
  letter->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(8, 0)));
  compositor.renderFrame(visibleViewport, visibleSurface);
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_EQ(compositor.lastRenderFrameStats().textureAllocationFailureCount, 0);
  EXPECT_EQ(compositor.lastRenderFrameStats().cachedTileCount, 0)
      << "The first pointer move should translate the retained G tile without rebuilding static "
         "segments";
  const auto tiles = compositor.snapshotTilesForUpload();
  EXPECT_THAT(tiles, ::testing::Contains(::testing::Truly([](const CompositorTile& tile) {
                return tile.isDragTarget && tile.textureSnapshot != nullptr &&
                       tile.canvasFromBitmap.isTranslation() &&
                       tile.canvasFromBitmap.translation().x > 0.0;
              })));
}

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

TEST(CompositorGeodeSplashTest, SelectionBudgetRefusalDoesNotMakePartialTilesPresentable) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(1536, 1024);

  RendererGeode renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport visibleViewport;
  visibleViewport.size = Vector2d(1536, 1024);
  visibleViewport.devicePixelRatio = 1.0;
  compositor.renderFrame(visibleViewport);
  const auto previouslyPresentedTiles = compositor.snapshotTilesForUpload();
  ASSERT_GT(previouslyPresentedTiles.size(), 4u);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());

  const auto letter = document.querySelector("#letter-G");
  ASSERT_TRUE(letter.has_value());
  ASSERT_TRUE(
      compositor.promoteEntity(letter->unsafeEntityHandle().entity(), InteractionHint::Selection));
  renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/256,
                                      /*maximumBytes=*/24u * 1024u * 1024u);
  RenderViewport prewarmViewport;
  prewarmViewport.size = Vector2d(1536, 1024);
  prewarmViewport.devicePixelRatio = 1.0;
  compositor.renderFrame(prewarmViewport);

  EXPECT_GT(compositor.lastRenderFrameStats().textureAllocationFailureCount, 0);
  EXPECT_FALSE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_FALSE(previouslyPresentedTiles.empty())
      << "the last complete frame must remain available while selected tiles cannot be allocated";
}

}  // namespace
}  // namespace donner::svg::compositor
