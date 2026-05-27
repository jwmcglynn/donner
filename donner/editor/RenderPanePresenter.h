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
  Entity suppressedLayerEntity = entt::null;
  bool suppressDragTargetTiles = false;
  bool showOverlay = true;
  bool showFrameGraph = true;
};

/**
 * Return true when a composited tile should be drawn in the render pane.
 *
 * @param tile Tile view published by \ref GlTextureCache.
 * @param suppressedLayerEntity Promoted layer entity whose cached pixels should not be drawn while
 *   selection chrome remains visible. Null leaves all layer tiles eligible.
 * @param suppressDragTargetTiles True when the current selected element is `display:none` and
 *   legacy/metadata-missing elevated drag-target tiles should be hidden as a fallback.
 */
[[nodiscard]] bool ShouldPresentCompositedTile(const GlTextureCache::TileView& tile,
                                               Entity suppressedLayerEntity,
                                               bool suppressDragTargetTiles = false);

/// Draws the advanced editor render pane's image, overlay chrome, and frame graph.
class RenderPanePresenter {
public:
  void render(const RenderPanePresenterState& state) const;
};

}  // namespace donner::editor
