#include "donner/editor/RenderPanePresenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/Vector2.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/ViewportState.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(RenderPanePresenterTest, SuppressedDragTargetTileForSameLayerEntityIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(42);
  tile.isDragTarget = true;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, entt::null));
  EXPECT_FALSE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)));
}

TEST(RenderPanePresenterTest, SuppressedLayerEntityTileIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(42);
  tile.isDragTarget = false;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)));
}

TEST(RenderPanePresenterTest, SuppressedImmediateEntityTileIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Immediate;
  tile.layerEntity = static_cast<Entity>(42);
  tile.isDragTarget = false;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)))
      << "Immediate-mode promoted layers still carry layerEntity ownership, so deleting or hiding "
         "the selection must suppress them just like cached layer tiles.";
}

TEST(RenderPanePresenterTest, HiddenSelectionSuppressionKeepsDifferentDragTargetTileVisible) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(7);
  tile.isDragTarget = true;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)))
      << "Suppressing stale pixels for a display:none selection must not hide the freshly "
         "selected/drag-target layer for a different visible entity.";
}

TEST(RenderPanePresenterTest, CurrentDisplayNoneSelectionSuppressesDragTargetTileFallback) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = entt::null;
  tile.isDragTarget = true;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42),
                                           /*suppressDragTargetTiles=*/true))
      << "While the current selection is display:none, legacy elevated tiles without layer "
         "metadata should be hidden so the old selected shape disappears immediately.";
}

TEST(RenderPanePresenterTest, SuppressionDoesNotHideUnmatchedLayerTiles) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(7);
  tile.isDragTarget = false;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)));
}

TEST(RenderPanePresenterTest, SuppressionDoesNotHideSegmentTiles) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Segment;
  tile.layerEntity = entt::null;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, static_cast<Entity>(42)));
}

TEST(RenderPanePresenterTest, MissingTextureIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = 0;
  tile.isDragTarget = false;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, entt::null));
}

TEST(RenderPanePresenterTest, SelectionPrewarmLayerMatchesGroupedActiveDragPreview) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(43);
  tile.isDragTarget = false;

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = static_cast<Entity>(42),
      .extraEntities = {static_cast<Entity>(43)},
      .translation = Vector2d(8.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(8.0, 0.0)),
      .dragGeneration = 5,
  };

  EXPECT_TRUE(TileMatchesActiveDragPreview(tile, activeDrag))
      << "Grouped selection prewarm tiles are valid drag targets as soon as the active drag "
         "starts, "
         "even though the worker did not mark them as drag targets during the idle prewarm.";
}

TEST(RenderPanePresenterTest, ActiveDragPreviewMatchingHandlesPrimaryAndMissingPreview) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(42);
  tile.isDragTarget = false;

  EXPECT_FALSE(TileMatchesActiveDragPreview(tile, std::nullopt));

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = static_cast<Entity>(42),
      .extraEntities = {},
      .translation = Vector2d(8.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(8.0, 0.0)),
      .dragGeneration = 5,
  };
  EXPECT_TRUE(TileMatchesActiveDragPreview(tile, activeDrag));

  tile.layerEntity = static_cast<Entity>(7);
  EXPECT_FALSE(TileMatchesActiveDragPreview(tile, activeDrag));

  tile.layerEntity = entt::null;
  tile.isDragTarget = true;
  EXPECT_TRUE(TileMatchesActiveDragPreview(tile, activeDrag));
}

TEST(RenderPanePresenterTest, StaleDragTargetMetadataDoesNotMatchDifferentLiveEntity) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.kind = RenderResult::CompositedTile::Kind::Layer;
  tile.layerEntity = static_cast<Entity>(7);
  tile.isDragTarget = true;

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = static_cast<Entity>(42),
      .translation = Vector2d(8.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(8.0, 0.0)),
      .dragGeneration = 5,
  };

  EXPECT_FALSE(TileMatchesActiveDragPreview(tile, activeDrag));
}

TEST(RenderPanePresenterTest, OverviewTilesAreOnlyPresentedUnderViewportBoundedActiveTiles) {
  GlTextureCache::TileView overviewTile;
  overviewTile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));

  const std::array<GlTextureCache::TileView, 1> overviewTiles{overviewTile};

  EXPECT_TRUE(ShouldPresentOverviewTiles(/*activeTilesViewportBounded=*/true, overviewTiles));
  EXPECT_FALSE(ShouldPresentOverviewTiles(/*activeTilesViewportBounded=*/false, overviewTiles))
      << "A retained overview is a fallback for viewport-bounded active tiles only. Drawing it "
         "under a full-document active tile set can show stale pre-drag pixels underneath a "
         "rerendered transform target.";
}

TEST(RenderPanePresenterTest, EmptyOverviewTileSetIsNotPresented) {
  EXPECT_FALSE(ShouldPresentOverviewTiles(
      /*activeTilesViewportBounded=*/true, std::span<const GlTextureCache::TileView>()));
}

TEST(RenderPanePresenterTest, FramebufferCheckerboardMatchesCanvasCheckerSize) {
  EXPECT_DOUBLE_EQ(kFramebufferCheckerboardSize, 16.0)
      << "The direct framebuffer presenter must use the same checkerboard cell size as the "
         "regular canvas presenter, otherwise deleting an opaque background switches the visible "
         "checkerboard scale when direct WGPU presentation takes over.";
}

TEST(RenderPanePresenterTest, PresentedTileQuadIntersectingPaneIsVisible) {
  PresentedTileQuad quad;
  quad.topLeft = Vector2d(10.0, 10.0);
  quad.topRight = Vector2d(30.0, 10.0);
  quad.bottomRight = Vector2d(30.0, 30.0);
  quad.bottomLeft = Vector2d(10.0, 30.0);

  EXPECT_TRUE(PresentedTileQuadIntersectsScreenRect(
      quad, Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/20.0, /*height=*/20.0)));
}

TEST(RenderPanePresenterTest, PresentedTileQuadOutsidePaneIsCulled) {
  PresentedTileQuad quad;
  quad.topLeft = Vector2d(30.0, 10.0);
  quad.topRight = Vector2d(40.0, 10.0);
  quad.bottomRight = Vector2d(40.0, 20.0);
  quad.bottomLeft = Vector2d(30.0, 20.0);

  EXPECT_FALSE(PresentedTileQuadIntersectsScreenRect(
      quad, Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/20.0, /*height=*/20.0)));
}

TEST(RenderPanePresenterTest, PresentedTileQuadTouchingPaneEdgeIsCulled) {
  PresentedTileQuad quad;
  quad.topLeft = Vector2d(20.0, 5.0);
  quad.topRight = Vector2d(30.0, 5.0);
  quad.bottomRight = Vector2d(30.0, 15.0);
  quad.bottomLeft = Vector2d(20.0, 15.0);

  EXPECT_FALSE(PresentedTileQuadIntersectsScreenRect(
      quad, Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/20.0, /*height=*/20.0)));
}

TEST(RenderPanePresenterTest, PresentedTileQuadRejectsNonFiniteAndEmptyScreenRects) {
  PresentedTileQuad quad;
  quad.topLeft = Vector2d(10.0, 10.0);
  quad.topRight = Vector2d(30.0, 10.0);
  quad.bottomRight = Vector2d(30.0, 30.0);
  quad.bottomLeft = Vector2d(10.0, 30.0);

  PresentedTileQuad nonFiniteQuad = quad;
  nonFiniteQuad.bottomRight.x = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(PresentedTileQuadIntersectsScreenRect(
      nonFiniteQuad, Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/40.0, /*height=*/40.0)));

  EXPECT_FALSE(PresentedTileQuadIntersectsScreenRect(
      quad, Box2d(Vector2d(20.0, 0.0), Vector2d(20.0, 40.0))));
  EXPECT_FALSE(PresentedTileQuadIntersectsScreenRect(
      quad, Box2d(Vector2d(0.0, 20.0), Vector2d(40.0, 20.0))));
}

TEST(RenderPanePresenterTest, PresentedImageClipRectIntersectsPaneWithArtboard) {
  const std::optional<Box2d> clip = PresentedImageClipRect(
      Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/100.0, /*height=*/80.0),
      Box2d::FromXYWH(/*x=*/20.0, /*y=*/10.0, /*width=*/120.0, /*height=*/40.0));

  ASSERT_TRUE(clip.has_value());
  EXPECT_EQ(*clip, Box2d::FromXYWH(/*x=*/20.0, /*y=*/10.0, /*width=*/80.0, /*height=*/40.0));
}

TEST(RenderPanePresenterTest, PresentedImageClipRectRejectsDisjointArtboard) {
  EXPECT_FALSE(PresentedImageClipRect(
                   Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/100.0, /*height=*/80.0),
                   Box2d::FromXYWH(/*x=*/100.0, /*y=*/10.0, /*width=*/40.0, /*height=*/40.0))
                   .has_value());
}

TEST(RenderPanePresenterTest, PresentedImageClipRectRejectsNonFiniteAndEmptyIntersections) {
  const Box2d paneRect = Box2d::FromXYWH(/*x=*/0.0, /*y=*/0.0, /*width=*/100.0,
                                         /*height=*/80.0);
  Box2d nonFiniteImage = Box2d::FromXYWH(/*x=*/20.0, /*y=*/10.0, /*width=*/40.0,
                                         /*height=*/20.0);
  nonFiniteImage.bottomRight.y = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(PresentedImageClipRect(paneRect, nonFiniteImage).has_value());

  EXPECT_FALSE(PresentedImageClipRect(paneRect, Box2d(Vector2d(20.0, 10.0), Vector2d(80.0, 10.0)))
                   .has_value());
  EXPECT_FALSE(PresentedImageClipRect(paneRect, Box2d(Vector2d(20.0, 10.0), Vector2d(20.0, 60.0)))
                   .has_value());
}

}  // namespace
}  // namespace donner::editor
