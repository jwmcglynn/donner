#include "donner/editor/RenderPanePresenter.h"

#include <cstdint>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(RenderPanePresenterTest, SuppressedDragTargetTileIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.isDragTarget = true;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, /*suppressDragTargetTiles=*/false));
  EXPECT_FALSE(ShouldPresentCompositedTile(tile, /*suppressDragTargetTiles=*/true));
}

TEST(RenderPanePresenterTest, SuppressionDoesNotHideNonDragTargetTiles) {
  GlTextureCache::TileView tile;
  tile.texture = static_cast<ImTextureID>(static_cast<std::uintptr_t>(7));
  tile.isDragTarget = false;

  EXPECT_TRUE(ShouldPresentCompositedTile(tile, /*suppressDragTargetTiles=*/true));
}

TEST(RenderPanePresenterTest, MissingTextureIsNotPresented) {
  GlTextureCache::TileView tile;
  tile.texture = 0;
  tile.isDragTarget = false;

  EXPECT_FALSE(ShouldPresentCompositedTile(tile, /*suppressDragTargetTiles=*/false));
}

}  // namespace
}  // namespace donner::editor
