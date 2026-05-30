#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::editor {

struct RenderPanePresenterState {
  const ViewportState& viewport;
  const FrameHistory& frameHistory;
  const GlTextureCache& textures;
  const std::optional<SelectionChromeSnapshot>& immediateOverlaySnapshot;
  const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview;
  const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview;
  Vector2d contentRegion = Vector2d::Zero();
  Entity suppressedLayerEntity = entt::null;
  bool suppressDragTargetTiles = false;
  bool showOverlay = true;
  bool drawImmediateOverlay = true;
  bool showFrameGraph = true;
};

/// CPU cost counters produced while presenting the render pane.
struct RenderPanePresenterCost {
  /// Milliseconds spent issuing immediate overlay draw-list commands.
  double immediateOverlayDrawMs = 0.0;
};

/**
 * Return true when a composited tile should be drawn in the render pane.
 *
 * @param tile Tile view published by \ref GlTextureCache.
 * @param suppressedLayerEntity Promoted entity whose cached or immediate pixels should not be drawn
 *   while selection chrome remains visible. Null leaves all entity-owned tiles eligible.
 * @param suppressDragTargetTiles True when the current selected element is `display:none` and
 *   legacy/metadata-missing elevated drag-target tiles should be hidden as a fallback.
 */
[[nodiscard]] bool ShouldPresentCompositedTile(const GlTextureCache::TileView& tile,
                                               Entity suppressedLayerEntity,
                                               bool suppressDragTargetTiles = false);

/**
 * Return true when @p tile should receive the current active-drag transform.
 *
 * Selection prewarm renders can publish layer tiles before the user starts dragging. Those tiles
 * are valid drag presentation candidates even though their worker-side `isDragTarget` bit was
 * false at prewarm time.
 *
 * @param tile Tile view published by \ref GlTextureCache.
 * @param activeDragPreview Active drag preview driving presenter-side transforms.
 */
[[nodiscard]] bool TileMatchesActiveDragPreview(
    const GlTextureCache::TileView& tile,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview);

/**
 * Return true when the presenter can move the active drag target in the current tile set.
 *
 * @param textures Presentation texture cache.
 * @param activeDragPreview Active drag preview driving presenter-side transforms.
 * @param suppressedLayerEntity Promoted entity hidden from presentation.
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

class RenderPanePresenter {
public:
  /**
   * Draw the advanced editor render pane's image, overlay chrome, and frame graph.
   *
   * @param state Presentation inputs for the current UI frame.
   * @return CPU cost counters for work issued by the presenter.
   */
  [[nodiscard]] RenderPanePresenterCost render(const RenderPanePresenterState& state) const;
};

}  // namespace donner::editor
