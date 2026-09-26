#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/CompositorControllerInternal.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg::compositor {
namespace {

// Geode's flat and texture-composited paths can differ at antialiased SVG contour edges. Use the
// shared renderer-suite perceptual threshold there, while requiring zero non-AA mismatches;
// tile-vs-tile damage and demotion comparisons below remain strict identity.
constexpr auto kGeodeFlatReferenceParams =
    editor::tests::ApprovedPixelToleranceParams(0.02f, 0, /*includeAntiAliasing=*/false);

void ExpectOffArtboardPixels(const RendererBitmap& fullTile, int rows, const char* label) {
  ASSERT_FALSE(fullTile.empty());
  ASSERT_GT(rows, 0);
  ASSERT_LE(rows, fullTile.dimensions.y);
  RendererBitmap crop;
  crop.dimensions = Vector2i(fullTile.dimensions.x, rows);
  crop.rowBytes = static_cast<std::size_t>(crop.dimensions.x) * 4u;
  crop.alphaType = fullTile.alphaType;
  crop.pixels.resize(crop.rowBytes * static_cast<std::size_t>(rows));
  for (int y = 0; y < rows; ++y) {
    std::copy_n(fullTile.pixels.data() + static_cast<std::size_t>(y) * fullTile.rowBytes,
                crop.rowBytes, crop.pixels.data() + static_cast<std::size_t>(y) * crop.rowBytes);
  }
  RendererBitmap transparent = crop;
  std::fill(transparent.pixels.begin(), transparent.pixels.end(), 0u);
  int changedPixels = -1;
  editor::tests::CompareBitmapToBitmap(crop, transparent, label,
                                       editor::tests::BitmapGoldenCompareParams{
                                           .threshold = 0.0f,
                                           .maxMismatchedPixels = std::numeric_limits<int>::max(),
                                           .includeAntiAliasing = true,
                                       },
                                       &changedPixels);
  if (changedPixels <= 100) {
    // The failing path emits the shared helper's actual/expected/diff/side-by-side artifacts.
    editor::tests::CompareBitmapToBitmap(crop, transparent, label,
                                         editor::tests::PixelmatchIdentityParams());
  }
  EXPECT_GT(changedPixels, 100) << label << " must retain source pixels above the artboard";
}

TEST(CompositorGeodeSplashTest, OffArtboardShineRetainsSourcePixelsInSelectedTile) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("donner_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(892, 512);
  RendererUtils::prepareDocumentForRendering(document, /*verbose=*/false, warnings);

  const auto shine = document.querySelector("#Background_shine");
  ASSERT_TRUE(shine.has_value());
  const auto ellipse = document.querySelector("#Background_shine ellipse");
  ASSERT_TRUE(ellipse.has_value());
  const Entity shineEntity = shine->unsafeEntityHandle().entity();
  Renderer renderer;
  RenderViewport viewport;
  viewport.size = Vector2d(892, 512);
  viewport.devicePixelRatio = 1.0;
  RendererDriver boundsDriver(renderer);
  const auto [firstEntity, lastEntity] =
      std::pair{shineEntity, ellipse->unsafeEntityHandle().entity()};
  const auto clipped = boundsDriver.computeEntityRangeBounds(document.registry(), firstEntity,
                                                             lastEntity, viewport, Transform2d());
  const auto full = boundsDriver.computeEntityRangeBounds(
      document.registry(), firstEntity, lastEntity, viewport, Transform2d(),
      RendererDriver::EntityRangeBoundsOptions{.clipToCanvas = false});
  ASSERT_TRUE(clipped.has_value());
  ASSERT_TRUE(full.has_value());
  EXPECT_GE(clipped->topLeft.y, 0.0);
  EXPECT_LT(full->topLeft.y, -200.0);

  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.promoteEntity(shineEntity, InteractionHint::Selection));
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());
  const auto selectedTiles = compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  const auto selectedTile = std::find_if(
      selectedTiles.begin(), selectedTiles.end(),
      [shineEntity](const CompositorTile& tile) { return tile.layerEntity == shineEntity; });
  ASSERT_NE(selectedTile, selectedTiles.end());
  ASSERT_NE(selectedTile->textureSnapshot, nullptr);
  EXPECT_LT(selectedTile->canvasOffsetPx.y, -200.0);
  const RendererBitmap tileBitmap = selectedTile->textureSnapshot->takeSnapshot();
  ASSERT_FALSE(tileBitmap.empty());
  const int formerlyOffArtboardRows =
      std::min(tileBitmap.dimensions.y, static_cast<int>(-selectedTile->canvasOffsetPx.y));
  ExpectOffArtboardPixels(tileBitmap, formerlyOffArtboardRows, "shine_parent_off_artboard");

  ASSERT_TRUE(compositor.promoteEntity(shineEntity, InteractionHint::ActiveDrag));
  shine->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(0, 300)));
  compositor.renderFrame(viewport);
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_EQ(compositor.lastRenderFrameStats().cachedTileCount, 0)
      << "The first downward drag must move the complete selected bitmap without rerendering";
  const auto draggedTiles = compositor.snapshotTilesForUpload();
  EXPECT_THAT(draggedTiles, ::testing::Contains(::testing::Truly([shineEntity](const auto& tile) {
                return tile.layerEntity == shineEntity && tile.isDragTarget &&
                       tile.canvasFromBitmap.translation().y >= 300.0;
              })));

  auto referenceParsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(referenceParsed.hasError());
  SVGDocument referenceDocument = std::move(referenceParsed.result());
  referenceDocument.setCanvasSize(892, 512);
  const auto referenceShine = referenceDocument.querySelector("#Background_shine");
  ASSERT_TRUE(referenceShine.has_value());
  referenceShine->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(0, 300)));
  Renderer referenceRenderer;
  RendererDriver(referenceRenderer).draw(referenceDocument, viewport, Transform2d());
  const RendererBitmap actual = renderer.takeSnapshot();
  const RendererBitmap expected = referenceRenderer.takeSnapshot();
  editor::tests::CompareBitmapToBitmap(actual, expected, "shine_full_object_drag",
                                       kGeodeFlatReferenceParams);
}

TEST(CompositorGeodeSplashTest, OffArtboardShineEllipseChildAlsoKeepsFullSelectedTile) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("donner_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(892, 512);
  Renderer renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport viewport;
  viewport.size = Vector2d(892, 512);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);

  const auto ellipse = document.querySelector("#Background_shine ellipse");
  ASSERT_TRUE(ellipse.has_value());
  const Entity entity = ellipse->unsafeEntityHandle().entity();
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::Selection));
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());
  const auto tiles = compositor.snapshotTilesForUpload();
  const auto selectedTile = std::find_if(tiles.begin(), tiles.end(), [entity](const auto& tile) {
    return tile.layerEntity == entity;
  });
  ASSERT_NE(selectedTile, tiles.end());
  ASSERT_NE(selectedTile->textureSnapshot, nullptr);
  EXPECT_LT(selectedTile->canvasOffsetPx.y, -200.0);
  const RendererBitmap childBitmap = selectedTile->textureSnapshot->takeSnapshot();
  ASSERT_FALSE(childBitmap.empty());
  const int negativeRows =
      std::min(childBitmap.dimensions.y, static_cast<int>(-selectedTile->canvasOffsetPx.y));
  ExpectOffArtboardPixels(childBitmap, negativeRows, "shine_ellipse_off_artboard");

  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::ActiveDrag));
  ellipse->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(0, 300)));
  compositor.renderFrame(viewport);
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_EQ(compositor.lastRenderFrameStats().cachedTileCount, 0);
  EXPECT_THAT(compositor.snapshotTilesForUpload(),
              ::testing::Contains(::testing::Truly([entity](const auto& tile) {
                return tile.layerEntity == entity && tile.isDragTarget &&
                       tile.canvasFromBitmap.translation().y >= 300.0;
              })));
}

TEST(CompositorGeodeSplashTest, UnsupportedOffArtboardMarkerUsesOwningTiles) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"
                  viewBox="0 0 100 100">
        <defs><marker id="arrow" markerWidth="8" markerHeight="8" refX="4" refY="4">
          <path d="M0 0 L8 4 L0 8 Z" fill="black"/>
        </marker></defs>
        <path id="outside" d="M12 -24 L42 24" fill="none" stroke="black"
              marker-end="url(#arrow)"/>
      </svg>)svg",
      warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(100, 100);
  Renderer renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  const RenderViewport viewport{Vector2d(100, 100)};
  compositor.renderFrame(viewport);
  const auto previousTiles = compositor.snapshotTilesForUpload();
  ASSERT_FALSE(previousTiles.empty());
  const auto outside = document.querySelector("#outside");
  ASSERT_TRUE(outside.has_value());
  EXPECT_EQ(
      compositor.promoteEntity(outside->unsafeEntityHandle().entity(), InteractionHint::Selection),
      CompositorController::PromoteResult::OwningTilesRequired)
      << "Unknown marker extent must not be presented as a viewport-clipped selected tile";
  EXPECT_FALSE(compositor.isPromoted(outside->unsafeEntityHandle().entity()));
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation())
      << "The prior complete owning-tile frame remains available after refusal";
}

TEST(CompositorGeodeSplashTest, SelectedParentOwnsMaskedDescendantsWithoutDoublePaint) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(1536, 1024);
  auto referenceParsed = parser::SVGParser::ParseSVG(source.contents, warnings);
  ASSERT_FALSE(referenceParsed.hasError());
  SVGDocument referenceDocument = std::move(referenceParsed.result());
  referenceDocument.setCanvasSize(1536, 1024);
  Renderer referenceRenderer;

  Renderer renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport viewport;
  viewport.size = Vector2d(1536, 1024);
  viewport.devicePixelRatio = 1.0;
  const auto expectReferencePixels = [&](const char* phase) {
    const RendererBitmap actual = renderer.takeSnapshot();
    RendererDriver(referenceRenderer).draw(referenceDocument, viewport, Transform2d());
    const RendererBitmap expected = referenceRenderer.takeSnapshot();
    editor::tests::CompareBitmapToBitmap(actual, expected, phase, kGeodeFlatReferenceParams);
  };
  compositor.renderFrame(viewport);
  expectReferencePixels("cold baseline");
  const auto parent = document.querySelector("#geode");
  const auto maskedChild = document.querySelector("#shell-facets");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(maskedChild.has_value());
  const Entity parentEntity = parent->unsafeEntityHandle().entity();
  const Entity childEntity = maskedChild->unsafeEntityHandle().entity();
  ASSERT_NE(compositor.findLayerForTest(childEntity), nullptr);

  ASSERT_TRUE(compositor.promoteEntity(parentEntity, InteractionHint::Selection));
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());
  expectReferencePixels("selected parent");
  EXPECT_EQ(compositor.findLayerForTest(childEntity), nullptr);
  EXPECT_NE(compositor.findLayerForTest(parentEntity), nullptr);
  auto tiles = compositor.snapshotTilesForUpload();
  EXPECT_THAT(tiles, ::testing::Contains(::testing::Truly([parentEntity](const auto& tile) {
                return tile.layerEntity == parentEntity && tile.isDragTarget &&
                       tile.textureSnapshot != nullptr;
              })));
  EXPECT_THAT(tiles,
              ::testing::Not(::testing::Contains(::testing::Truly(
                  [childEntity](const auto& tile) { return tile.layerEntity == childEntity; }))));

  ASSERT_TRUE(compositor.promoteEntity(parentEntity, InteractionHint::ActiveDrag));
  parent->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(12, 4)));
  const auto referenceParent = referenceDocument.querySelector("#geode");
  ASSERT_TRUE(referenceParent.has_value());
  referenceParent->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(12, 4)));
  compositor.renderFrame(viewport);
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  expectReferencePixels("first parent drag");
  EXPECT_EQ(compositor.lastRenderFrameStats().cachedTileCount, 0)
      << "The masked descendants should travel in the retained parent tile on first motion";
  tiles = compositor.snapshotTilesForUpload();
  EXPECT_THAT(tiles, ::testing::Contains(::testing::Truly([parentEntity](const auto& tile) {
                return tile.layerEntity == parentEntity && tile.isDragTarget &&
                       tile.canvasFromBitmap.translation().x > 0.0;
              })));

  compositor.renderFrame(viewport);
  expectReferencePixels("steady parent drag");

  compositor.demoteEntity(parentEntity);
  compositor.renderFrame(viewport);
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_EQ(compositor.findLayerForTest(parentEntity), nullptr);
  EXPECT_NE(compositor.findLayerForTest(childEntity), nullptr)
      << "The mandatory mask layer must resume immediately after parent deselection";

  // The editor presents the compositor's texture tile set. Compare that exact user-facing
  // payload and ordering with a fresh compositor at the same document transform. The backend's
  // separate flattened main target is not used by the editor's Geode presentation path.
  CompositorController fresh(referenceDocument, referenceRenderer, config);
  fresh.renderFrame(viewport);
  const auto actualTiles = compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  const auto expectedTiles = fresh.snapshotTilesForUpload(CompositorTileBitmapPayload::All);
  ASSERT_EQ(actualTiles.size(), expectedTiles.size());
  for (std::size_t i = 0; i < actualTiles.size(); ++i) {
    const CompositorTile& actual = actualTiles[i];
    const CompositorTile& expected = expectedTiles[i];
    EXPECT_EQ(actual.layerEntity, expected.layerEntity) << i;
    EXPECT_EQ(actual.bitmapDims, expected.bitmapDims) << i;
    EXPECT_EQ(actual.canvasOffsetPx, expected.canvasOffsetPx) << i;
    EXPECT_EQ(actual.canvasFromBitmap, expected.canvasFromBitmap) << i;
    EXPECT_EQ(actual.immediate, expected.immediate) << i;
    const RendererBitmap actualBitmap =
        actual.textureSnapshot != nullptr ? actual.textureSnapshot->takeSnapshot() : actual.bitmap;
    const RendererBitmap expectedBitmap = expected.textureSnapshot != nullptr
                                              ? expected.textureSnapshot->takeSnapshot()
                                              : expected.bitmap;
    if (!actualBitmap.empty() && !expectedBitmap.empty()) {
      editor::tests::CompareBitmapToBitmap(actualBitmap, expectedBitmap,
                                           "restored_geode_tile_" + std::to_string(i),
                                           editor::tests::PixelmatchIdentityParams());
    }
  }
}

TEST(CompositorGeodeSplashTest, FailedExclusiveParentAllocationRestoresMandatoryChildren) {
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
  RenderViewport viewport;
  viewport.size = Vector2d(1536, 1024);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());
  const auto previousCompleteTiles = compositor.snapshotTilesForUpload();
  ASSERT_GT(previousCompleteTiles.size(), 4u);

  const auto parent = document.querySelector("#geode");
  const auto child = document.querySelector("#shell-facets");
  ASSERT_TRUE(parent.has_value());
  ASSERT_TRUE(child.has_value());
  const Entity parentEntity = parent->unsafeEntityHandle().entity();
  const Entity childEntity = child->unsafeEntityHandle().entity();
  ASSERT_TRUE(compositor.promoteEntity(parentEntity, InteractionHint::Selection));
  renderer.setSurfaceBudgetForTesting(/*maximumSurfaces=*/256,
                                      /*maximumBytes=*/1u * 1024u * 1024u);
  compositor.renderFrame(viewport);

  EXPECT_GT(compositor.lastRenderFrameStats().textureAllocationFailureCount, 0);
  EXPECT_FALSE(compositor.hasCompleteTileSetForPresentation())
      << "An incomplete exclusive frame must not replace the last complete editor presentation";
  EXPECT_FALSE(compositor.isPromoted(parentEntity));
  EXPECT_NE(compositor.findLayerForTest(childEntity), nullptr)
      << "Allocation refusal must restore the mandatory descendant assignment";
  EXPECT_EQ(compositor.promoteEntity(parentEntity, InteractionHint::Selection),
            CompositorController::PromoteResult::MemoryLimit)
      << "The same-size retry must not immediately reconstruct the failed exclusive tile";
  EXPECT_FALSE(previousCompleteTiles.empty());
}

TEST(CompositorGeodeSplashTest, MaskedCrystalFaceDragRepaintsOwningTileBeforeRelease) {
  const donner::tests::RequiredRunfile source =
      donner::tests::ReadRequiredRunfile("geode_splash.svg");
  DONNER_REQUIRE_RUNFILE(source);
  for (const char* selector : {"#central-crown-face-2", "#central-silhouette"}) {
    ParseWarningSink warnings = ParseWarningSink::Disabled();
    auto parsed = parser::SVGParser::ParseSVG(source.contents, warnings);
    ASSERT_FALSE(parsed.hasError());
    SVGDocument document = std::move(parsed.result());
    document.setCanvasSize(1536, 1024);
    auto referenceParsed = parser::SVGParser::ParseSVG(source.contents, warnings);
    ASSERT_FALSE(referenceParsed.hasError());
    SVGDocument referenceDocument = std::move(referenceParsed.result());
    referenceDocument.setCanvasSize(1536, 1024);

    Renderer renderer;
    CompositorConfig config;
    config.deferFirstFrameWarmup = false;
    CompositorController compositor(document, renderer, config);
    RenderViewport viewport;
    viewport.size = Vector2d(1536, 1024);
    viewport.devicePixelRatio = 1.0;
    compositor.renderFrame(viewport);
    const auto owner = document.querySelector("#cavity-artwork");
    const auto target = document.querySelector(selector);
    const auto referenceTarget = referenceDocument.querySelector(selector);
    ASSERT_TRUE(owner.has_value());
    ASSERT_TRUE(target.has_value());
    ASSERT_TRUE(referenceTarget.has_value());
    const Entity ownerEntity = owner->unsafeEntityHandle().entity();
    const CompositorLayer* ownerLayer = compositor.findLayerForTest(ownerEntity);
    ASSERT_NE(ownerLayer, nullptr);
    ASSERT_NE(ownerLayer->textureSnapshot(), nullptr);
    const Vector2i ownerDims = ownerLayer->textureSnapshot()->dimensions();
    EXPECT_LT(static_cast<std::int64_t>(ownerDims.x) * ownerDims.y,
              static_cast<std::int64_t>(viewport.size.x * viewport.size.y) / 4)
        << "The masked cavity must use conservative source bounds, not a full-canvas tile";
    const std::uint64_t ownerGeneration = ownerLayer->generation();
    EXPECT_EQ(compositor.promoteEntity(target->unsafeEntityHandle().entity(),
                                       InteractionHint::ActiveDrag),
              CompositorController::PromoteResult::OwningTilesRequired)
        << selector << " stays inside the shared cavity mask";

    target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(12, 0)));
    referenceTarget->cast<SVGGraphicsElement>().setTransform(
        Transform2d::Translate(Vector2d(12, 0)));
    compositor.renderFrame(viewport);
    ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation()) << selector;
    ownerLayer = compositor.findLayerForTest(ownerEntity);
    ASSERT_NE(ownerLayer, nullptr);
    std::cerr << "[GEODE-CAVITY] " << selector << " tile=" << ownerDims.x << "x" << ownerDims.y
              << " owner_raster_ms=" << ownerLayer->lastRasterizeMs()
              << " cached_frame_ms=" << compositor.lastRenderFrameStats().cachedRasterizeMs << "\n";
    EXPECT_GT(ownerLayer->generation(), ownerGeneration)
        << selector << " must update its owning masked tile while the pointer is held";
    EXPECT_EQ(compositor.lastRenderFrameStats().damagePatchTileCount, 1) << selector;
    EXPECT_LT(compositor.lastRenderFrameStats().damagePatchGeometryDraws, 400u)
        << selector << " must cull unrelated cavity facets from the damage render";
    Renderer freshOwnerRenderer;
    CompositorController freshOwner(referenceDocument, freshOwnerRenderer, config);
    freshOwner.renderFrame(viewport);
    const auto referenceOwner = referenceDocument.querySelector("#cavity-artwork");
    ASSERT_TRUE(referenceOwner.has_value());
    const CompositorLayer* freshOwnerLayer =
        freshOwner.findLayerForTest(referenceOwner->unsafeEntityHandle().entity());
    ASSERT_NE(freshOwnerLayer, nullptr);
    ASSERT_NE(ownerLayer->textureSnapshot(), nullptr);
    ASSERT_NE(freshOwnerLayer->textureSnapshot(), nullptr);
    EXPECT_EQ(ownerLayer->canvasOffset(), freshOwnerLayer->canvasOffset());
    editor::tests::CompareBitmapToBitmap(ownerLayer->textureSnapshot()->takeSnapshot(),
                                         freshOwnerLayer->textureSnapshot()->takeSnapshot(),
                                         std::string(selector) + "_damage_patch_vs_full_owner",
                                         editor::tests::PixelmatchIdentityParams());
    // EditorShellTest.GeodeMaskedCrystalFacesChangePixelsDuringHeldDrags proves visible first and
    // second held-frame motion. Here the exact owner texture must match a full re-raster even
    // though the incremental path draws only the changed rectangle.
  }
}

TEST(CompositorGeodeSplashTest, FilteredSiblingDisablesMaskedOwnerDamagePatch) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96">
      <defs>
        <mask id="m" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse"
              x="0" y="0" width="96" height="96">
          <rect width="96" height="96" fill="white"/>
        </mask>
        <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter>
      </defs>
      <g id="owner" mask="url(#m)">
        <rect id="target" x="10" y="10" width="20" height="20" fill="red"/>
        <circle id="filtered" cx="70" cy="70" r="12" fill="blue" filter="url(#blur)"/>
      </g>
    </svg>
  )svg";
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kSource, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(96, 96);
  auto referenceParsed = parser::SVGParser::ParseSVG(kSource, warnings);
  ASSERT_FALSE(referenceParsed.hasError());
  SVGDocument referenceDocument = std::move(referenceParsed.result());
  referenceDocument.setCanvasSize(96, 96);
  Renderer renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  const RenderViewport viewport{Vector2d(96, 96)};
  compositor.renderFrame(viewport);
  const auto owner = document.querySelector("#owner");
  const auto target = document.querySelector("#target");
  const auto referenceTarget = referenceDocument.querySelector("#target");
  ASSERT_TRUE(owner.has_value());
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(referenceTarget.has_value());
  const Entity ownerEntity = owner->unsafeEntityHandle().entity();
  const CompositorLayer* ownerLayer = compositor.findLayerForTest(ownerEntity);
  ASSERT_NE(ownerLayer, nullptr);
  const std::uint64_t generationBefore = ownerLayer->generation();

  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(8, 0)));
  referenceTarget->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(8, 0)));
  compositor.renderFrame(viewport);
  ownerLayer = compositor.findLayerForTest(ownerEntity);
  ASSERT_NE(ownerLayer, nullptr);
  EXPECT_GT(ownerLayer->generation(), generationBefore);
  EXPECT_EQ(compositor.lastRenderFrameStats().damagePatchTileCount, 0)
      << "A filtered sibling may sample outside the patch, so the whole masked owner must render";
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());

  Renderer referenceRenderer;
  RendererDriver(referenceRenderer).draw(referenceDocument, viewport, Transform2d());
  editor::tests::CompareBitmapToBitmap(renderer.takeSnapshot(), referenceRenderer.takeSnapshot(),
                                       "filtered_sibling_owner_fallback",
                                       kGeodeFlatReferenceParams);
}

TEST(CompositorGeodeSplashTest, FilteredMaskContentDisablesOwnerDamagePatch) {
  constexpr std::string_view kSource = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96">
      <defs>
        <filter id="blur"><feGaussianBlur stdDeviation="2"/></filter>
        <mask id="m" maskUnits="userSpaceOnUse" maskContentUnits="userSpaceOnUse"
              x="0" y="0" width="96" height="96">
          <g filter="url(#blur)"><rect width="96" height="96" fill="white"/></g>
        </mask>
      </defs>
      <g id="owner" mask="url(#m)">
        <rect id="target" x="10" y="10" width="20" height="20" fill="red"/>
        <rect x="40" y="40" width="20" height="20" fill="blue"/>
      </g>
    </svg>
  )svg";
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(kSource, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(96, 96);
  auto referenceParsed = parser::SVGParser::ParseSVG(kSource, warnings);
  ASSERT_FALSE(referenceParsed.hasError());
  SVGDocument referenceDocument = std::move(referenceParsed.result());
  referenceDocument.setCanvasSize(96, 96);
  Renderer renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  const RenderViewport viewport{Vector2d(96, 96)};
  compositor.renderFrame(viewport);
  const auto owner = document.querySelector("#owner");
  const auto target = document.querySelector("#target");
  const auto referenceTarget = referenceDocument.querySelector("#target");
  ASSERT_TRUE(owner.has_value());
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(referenceTarget.has_value());
  const Entity ownerEntity = owner->unsafeEntityHandle().entity();
  const CompositorLayer* ownerLayer = compositor.findLayerForTest(ownerEntity);
  ASSERT_NE(ownerLayer, nullptr);
  const std::uint64_t generationBefore = ownerLayer->generation();

  target->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(8, 0)));
  referenceTarget->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(Vector2d(8, 0)));
  compositor.renderFrame(viewport);
  ownerLayer = compositor.findLayerForTest(ownerEntity);
  ASSERT_NE(ownerLayer, nullptr);
  EXPECT_GT(ownerLayer->generation(), generationBefore);
  EXPECT_EQ(compositor.lastRenderFrameStats().damagePatchTileCount, 0)
      << "Filtered mask content can sample outside the damage viewport";
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  Renderer referenceRenderer;
  CompositorController fresh(referenceDocument, referenceRenderer, config);
  fresh.renderFrame(viewport);
  const auto freshOwner = referenceDocument.querySelector("#owner");
  ASSERT_TRUE(freshOwner.has_value());
  const CompositorLayer* freshLayer =
      fresh.findLayerForTest(freshOwner->unsafeEntityHandle().entity());
  ASSERT_NE(freshLayer, nullptr);
  ASSERT_NE(ownerLayer->textureSnapshot(), nullptr);
  ASSERT_NE(freshLayer->textureSnapshot(), nullptr);
  editor::tests::CompareBitmapToBitmap(
      ownerLayer->textureSnapshot()->takeSnapshot(), freshLayer->textureSnapshot()->takeSnapshot(),
      "filtered_mask_owner_full_fallback", editor::tests::PixelmatchIdentityParams());
}

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
                                      /*maximumBytes=*/8u * 1024u * 1024u);
  RenderViewport prewarmViewport;
  prewarmViewport.size = Vector2d(1536, 1024);
  prewarmViewport.devicePixelRatio = 1.0;
  compositor.renderFrame(prewarmViewport);

  EXPECT_GT(compositor.lastRenderFrameStats().textureAllocationFailureCount, 0);
  EXPECT_FALSE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_FALSE(previouslyPresentedTiles.empty())
      << "the last complete frame must remain available while selected tiles cannot be allocated";
}

TEST(CompositorGeodeSplashTest, ZoomedSelectionFallsBackToCompleteOwningTiles) {
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
  RenderViewport viewport;
  viewport.size = Vector2d(1536, 1024);
  viewport.devicePixelRatio = 1.0;
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());

  const auto letter = document.querySelector("#letter-G");
  ASSERT_TRUE(letter.has_value());
  const Entity entity = letter->unsafeEntityHandle().entity();
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::Selection));

  // Promotion used the overview geometry. The same raster dimensions at a new zoom can make the
  // full-object tile too large even though all owning/static tiles remain viewport bounded.
  const Transform2d zoom = Transform2d::Scale(40.0);
  const CompositorLayer* selectedLayer = compositor.findLayerForTest(entity);
  ASSERT_NE(selectedLayer, nullptr);
  const LayerRasterGeometry selectedAtZoom = ComputeLayerRasterGeometry(
      renderer, document.registry(), selectedLayer->firstEntity(), selectedLayer->lastEntity(),
      viewport, zoom, /*retainFullInteractionBounds=*/true);
  ASSERT_TRUE(selectedAtZoom.interactionBoundsRejected);
  compositor.renderFrame(viewport, zoom);
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_EQ(compositor.lastRenderFrameStats().textureAllocationFailureCount, 0);
  EXPECT_EQ(compositor.promoteEntity(entity, InteractionHint::Selection),
            CompositorController::PromoteResult::MemoryLimit);

  // A return to the overview can promote again, even though the raster dimensions are unchanged.
  compositor.renderFrame(viewport);
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_TRUE(compositor.promoteEntity(entity, InteractionHint::Selection));
}

TEST(CompositorGeodeSplashTest, FilteredDragCrossingArtboardReturnsToOwningTile) {
  constexpr std::string_view source = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="264" height="100">
  <defs><filter id="blur"><feGaussianBlur stdDeviation="4"/></filter></defs>
  <g id="glow" filter="url(#blur)">
    <circle cx="150" cy="50" r="20" fill="yellow"/>
  </g>
</svg>)svg";
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(264, 100);
  RendererGeode renderer;
  CompositorConfig config;
  config.deferFirstFrameWarmup = false;
  CompositorController compositor(document, renderer, config);
  RenderViewport viewport;
  viewport.size = Vector2d(264, 100);
  compositor.renderFrame(viewport);

  const auto glow = document.querySelector("#glow");
  ASSERT_TRUE(glow.has_value());
  const Entity entity = glow->unsafeEntityHandle().entity();
  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::Selection));
  compositor.renderFrame(viewport);
  ASSERT_TRUE(compositor.hasCompleteTileSetForPresentation());
  const RendererBitmap beforeDrag = renderer.takeSnapshot();
  ASSERT_FALSE(beforeDrag.empty());

  ASSERT_TRUE(compositor.promoteEntity(entity, InteractionHint::ActiveDrag));
  glow->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(100.0, 0.0));
  compositor.renderFrame(viewport);
  EXPECT_FALSE(compositor.isPromoted(entity));
  EXPECT_NE(compositor.findLayerForTest(entity), nullptr)
      << "the mandatory filtered owner must replace the full-object interaction tile";
  EXPECT_TRUE(compositor.hasCompleteTileSetForPresentation());
  EXPECT_EQ(compositor.lastRenderFrameStats().textureAllocationFailureCount, 0);

  // Compare the held frame against a fresh compositor in the same Geode mode. A nonempty but
  // stale mandatory owner would satisfy the topology checks above while showing old pixels.
  auto referenceParsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_FALSE(referenceParsed.hasError());
  SVGDocument referenceDocument = std::move(referenceParsed.result());
  referenceDocument.setCanvasSize(264, 100);
  const auto referenceGlow = referenceDocument.querySelector("#glow");
  ASSERT_TRUE(referenceGlow.has_value());
  referenceGlow->cast<SVGGraphicsElement>().setTransform(Transform2d::Translate(100.0, 0.0));
  RendererGeode referenceRenderer;
  CompositorController referenceCompositor(referenceDocument, referenceRenderer, config);
  referenceCompositor.renderFrame(viewport);
  // The first frame is a flat cold draw; advance to the cached-tile composition mode used by
  // the held compositor frame before comparing their final pixels.
  referenceCompositor.renderFrame(viewport);
  ASSERT_TRUE(referenceCompositor.hasCompleteTileSetForPresentation());
  const RendererBitmap heldFrame = renderer.takeSnapshot();
  const RendererBitmap freshFrame = referenceRenderer.takeSnapshot();
  ASSERT_FALSE(heldFrame.empty());
  ASSERT_FALSE(freshFrame.empty());
  int movedPixels = 0;
  editor::tests::CompareBitmapToBitmap(heldFrame, beforeDrag, "filtered_glow_held_move_signal",
                                       editor::tests::BitmapGoldenCompareParams{
                                           .threshold = 0.0f,
                                           .maxMismatchedPixels = std::numeric_limits<int>::max(),
                                           .includeAntiAliasing = true,
                                       },
                                       &movedPixels);
  EXPECT_GT(movedPixels, 100);
  editor::tests::CompareBitmapToBitmap(heldFrame, freshFrame, "filtered_glow_held_vs_fresh",
                                       editor::tests::PixelmatchIdentityParams());

  const CompositorLayer* heldOwner = compositor.findLayerForTest(entity);
  const CompositorLayer* freshOwner =
      referenceCompositor.findLayerForTest(referenceGlow->unsafeEntityHandle().entity());
  ASSERT_NE(heldOwner, nullptr);
  ASSERT_NE(freshOwner, nullptr);
  ASSERT_NE(heldOwner->textureSnapshot(), nullptr);
  ASSERT_NE(freshOwner->textureSnapshot(), nullptr);
  EXPECT_EQ(heldOwner->canvasOffset(), freshOwner->canvasOffset());
  editor::tests::CompareBitmapToBitmap(
      heldOwner->textureSnapshot()->takeSnapshot(), freshOwner->textureSnapshot()->takeSnapshot(),
      "filtered_glow_held_owner_vs_fresh", editor::tests::PixelmatchIdentityParams());
  EXPECT_EQ(compositor.promoteEntity(entity, InteractionHint::ActiveDrag),
            CompositorController::PromoteResult::OwningTilesRequired);
}

}  // namespace
}  // namespace donner::svg::compositor
