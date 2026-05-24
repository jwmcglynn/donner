#pragma once
/// @file

#include <optional>

#include "donner/editor/GlTextureCache.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::editor {

struct RenderPanePresenterState {
  const ViewportState& viewport;
  const FrameHistory& frameHistory;
  const GlTextureCache& textures;
  const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview;
  const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview;
  const std::optional<SelectTool::ActiveDragPreview>& overlayDragPreview;
  Vector2d contentRegion = Vector2d::Zero();
  bool suppressDragTargetTiles = false;
};

/**
 * Return true when a composited tile should be drawn in the render pane.
 *
 * @param tile Tile view published by \ref GlTextureCache.
 * @param suppressDragTargetTiles True when selection chrome should remain visible but the cached
 *   promoted selection layer should not be drawn.
 */
[[nodiscard]] bool ShouldPresentCompositedTile(const GlTextureCache::TileView& tile,
                                               bool suppressDragTargetTiles);

/// Draws the advanced editor render pane's image, overlay chrome, and frame graph.
class RenderPanePresenter {
public:
  void render(const RenderPanePresenterState& state) const;
};

}  // namespace donner::editor
