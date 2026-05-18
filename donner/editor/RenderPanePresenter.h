#pragma once
/// @file

#include <optional>

#include "donner/editor/ExperimentalDragPresentation.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::editor {

struct RenderPanePresenterState {
  const ViewportState& viewport;
  const FrameHistory& frameHistory;
  const GlTextureCache& textures;
  const ExperimentalDragPresentation& experimentalDragPresentation;
  const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview;
  const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview;
  Vector2d contentRegion = Vector2d::Zero();
  bool experimentalMode = false;
};

/**
 * Resolve the drag translation to use when drawing a composited tile.
 *
 * @param tile Cached composited tile view.
 * @param activeDragPreview Live drag preview, when a drag is currently active.
 * @param displayedDragPreview Preview state selected by \ref ExperimentalDragPresentation.
 * @return Translation to add to the tile's canvas offset.
 */
[[nodiscard]] Vector2d ResolveCompositedTileDragTranslation(
    const GlTextureCache::TileView& tile,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview);

/// Draws the advanced editor render pane's image, overlay chrome, and frame graph.
class RenderPanePresenter {
public:
  void render(const RenderPanePresenterState& state) const;
};

}  // namespace donner::editor
