#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::editor {

struct RenderPanePresenterState {
  const ViewportState& viewport;
  const FrameHistory& frameHistory;
  const GlTextureCache& textures;
  const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview;
  const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview;
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

/**
 * Return true when the presenter can move the active drag target in the current tile set.
 *
 * @param textures Presentation texture cache.
 * @param activeDragPreview Active drag preview driving presenter-side transforms.
 * @param suppressedLayerEntity Promoted layer entity hidden from presentation.
 * @param suppressDragTargetTiles True when drag target tiles are globally hidden.
 */
[[nodiscard]] bool HasPresentableDragTargetTile(
    const GlTextureCache& textures,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    Entity suppressedLayerEntity, bool suppressDragTargetTiles = false);

/**
 * Return true when a presented tile quad has visible overlap with a screen rect.
 *
 * @param tileQuad Output-space tile quad from \ref ComputePresentedTileQuad.
 * @param screenRect Screen-space clip rect, usually the render pane bounds.
 */
[[nodiscard]] bool PresentedTileQuadIntersectsScreenRect(const PresentedTileQuad& tileQuad,
                                                         const Box2d& screenRect);

/**
 * Return the screen-space clip rect for presented document pixels.
 *
 * @param paneRect Screen-space render-pane bounds.
 * @param imageRect Screen-space artboard/image bounds.
 */
[[nodiscard]] std::optional<Box2d> PresentedImageClipRect(const Box2d& paneRect,
                                                          const Box2d& imageRect);

/// Draws the advanced editor render pane's image, overlay chrome, and frame graph.
class RenderPanePresenter {
public:
  void render(const RenderPanePresenterState& state) const;
};

}  // namespace donner::editor
