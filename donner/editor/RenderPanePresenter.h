#pragma once
/// @file

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Path.h"
#include "donner/base/Vector2.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/MenuBarPresenter.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportInteractionController.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

/// Transparency checkerboard cell size, in logical pixels. Only the Geode
/// framebuffer pass draws the checkerboard; there is no draw-list fallback.
inline constexpr double kFramebufferCheckerboardSize = 16.0;

struct RenderPanePresenterState {
  /// Live viewport for this frame. Owns pane geometry: the pane rect, the
  /// content region, and everything anchored to the window rather than to the
  /// document.
  const ViewportState& viewport;
  /// Viewport the document pixels presented this frame are actually placed
  /// with, or null when that is the live viewport.
  ///
  /// A worker-owned surface is positioned with the viewport its accepted epoch
  /// was rasterized against, which can be one or more worker frames behind the
  /// live one. Everything drawn in document space - the presented image clip
  /// rect, tile quads, and the compositor tile overlay - must use that same
  /// transform, or it annotates pixels that are no longer underneath it.
  const ViewportState* presentedDocumentViewport = nullptr;
  const FrameHistory& frameHistory;
  const GlTextureCache& textures;
  const std::optional<SelectionChromeSnapshot>& immediateOverlaySnapshot;
  const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview;
  const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview;
  Vector2d contentRegion = Vector2d::Zero();
  Entity suppressedLayerEntity = entt::null;
  bool suppressDragTargetTiles = false;
  bool documentPresentedDirectly = false;
  bool compositorTileOverlay = false;
  PerfOverlayMode perfOverlayMode = PerfOverlayMode::Off;
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
 * Return true when retained overview tiles should be drawn behind the active tile set.
 *
 * Overview infill is only coherent under viewport-bounded active tiles. Full-document active tiles
 * already represent the current presentation, so drawing an older retained overview underneath can
 * reintroduce stale pixels during a transform drag.
 *
 * @param activeTilesViewportBounded True when the active tile set covers only the viewport.
 * @param overviewTiles Retained full-document overview tiles.
 */
[[nodiscard]] bool ShouldPresentOverviewTiles(
    bool activeTilesViewportBounded, std::span<const GlTextureCache::TileView> overviewTiles);

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
   * Draw the advanced editor render pane's composited document tiles and, when
   * enabled, the performance overlay (compact FPS pill or full frame graph).
   *
   * Selection chrome is not drawn here: it is drawn immediately onto the window framebuffer
   * after the document tiles and before ImGui, so it shares the tiles' transform exactly.
   *
   * @param state Presentation inputs for the current UI frame.
   */
  void render(const RenderPanePresenterState& state) const;
};

}  // namespace donner::editor
