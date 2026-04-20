#pragma once
/// @file

#include <optional>

#include "donner/editor/ExperimentalDragPresentation.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/backend_lib/SelectTool.h"
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

/// Draws the advanced editor render pane's image, overlay chrome, and frame graph.
class RenderPanePresenter {
public:
  void render(const RenderPanePresenterState& state) const;
};

}  // namespace donner::editor
