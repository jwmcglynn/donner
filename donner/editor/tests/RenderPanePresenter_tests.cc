#include "donner/editor/RenderPanePresenter.h"

#include <optional>

#include "donner/base/EcsRegistry.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(RenderPanePresenterTest, ActiveDragTargetUsesLivePreviewTranslation) {
  GlTextureCache::TileView tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const Entity entity{123};
  const std::optional<SelectTool::ActiveDragPreview> active =
      SelectTool::ActiveDragPreview{.entity = entity, .translation = Vector2d(7.0, 8.0)};
  const std::optional<SelectTool::ActiveDragPreview> displayed =
      SelectTool::ActiveDragPreview{.entity = entity, .translation = Vector2d(7.0, 8.0)};

  EXPECT_EQ(ResolveCompositedTileDragTranslation(tile, active, displayed), Vector2d(7.0, 8.0));
}

TEST(RenderPanePresenterTest, IdleDragTargetKeepsCachedTileTranslation) {
  GlTextureCache::TileView tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const Entity entity{123};
  const std::optional<SelectTool::ActiveDragPreview> active = std::nullopt;
  const std::optional<SelectTool::ActiveDragPreview> displayed =
      SelectTool::ActiveDragPreview{.entity = entity, .translation = Vector2d::Zero()};

  EXPECT_EQ(ResolveCompositedTileDragTranslation(tile, active, displayed), Vector2d(1.0, 2.0));
}

TEST(RenderPanePresenterTest, TargetSwitchKeepsCachedTileTranslation) {
  GlTextureCache::TileView tile;
  tile.isDragTarget = true;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const std::optional<SelectTool::ActiveDragPreview> active =
      SelectTool::ActiveDragPreview{.entity = Entity(456), .translation = Vector2d(7.0, 8.0)};
  const std::optional<SelectTool::ActiveDragPreview> displayed =
      SelectTool::ActiveDragPreview{.entity = Entity(123), .translation = Vector2d::Zero()};

  EXPECT_EQ(ResolveCompositedTileDragTranslation(tile, active, displayed), Vector2d(1.0, 2.0));
}

TEST(RenderPanePresenterTest, NonDragTilesKeepCachedTileTranslation) {
  GlTextureCache::TileView tile;
  tile.isDragTarget = false;
  tile.dragTranslationDoc = Vector2d(1.0, 2.0);

  const Entity entity{123};
  const std::optional<SelectTool::ActiveDragPreview> active =
      SelectTool::ActiveDragPreview{.entity = entity, .translation = Vector2d(7.0, 8.0)};
  const std::optional<SelectTool::ActiveDragPreview> displayed =
      SelectTool::ActiveDragPreview{.entity = entity, .translation = Vector2d(7.0, 8.0)};

  EXPECT_EQ(ResolveCompositedTileDragTranslation(tile, active, displayed), Vector2d(1.0, 2.0));
}

}  // namespace
}  // namespace donner::editor
