#include "donner/editor/RenderPanePresenter.h"

#include <cstdint>

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

}  // namespace
}  // namespace donner::editor
