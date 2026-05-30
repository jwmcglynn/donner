#include "donner/editor/RenderPanePresenter.h"

#include <cstdint>
#include <optional>

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

}  // namespace
}  // namespace donner::editor
