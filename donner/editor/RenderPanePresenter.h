#pragma once
/// @file

#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Path.h"
#include "donner/base/Vector2.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportInteractionController.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

/// One screen-space polyline subpath produced by flattening a document-space
/// overlay path for the selection-chrome stroke.
struct OverlayStrokePolyline {
  /// Flattened screen-space points (logical pixels) for this subpath.
  std::vector<Vector2d> points;
  /// True when the originating subpath ended with a `ClosePath` verb.
  bool closed = false;
};

/// Selection-chrome miter limit, mirroring the Donner renderer's overlay stroke
/// (`OverlayRenderer.cc` `MakeSelectionStrokePaint`) and the SVG default. A
/// joint sharper than this is beveled instead of mitered so the ImGui overlay
/// stroke never grows a runaway miter spike (the QA "flare").
inline constexpr double kOverlayStrokeMiterLimit = 4.0;

/**
 * Flatten a document-space overlay path into screen-space polyline subpaths,
 * tessellating quadratic and cubic Béziers exactly the way the selection-chrome
 * stroke does (ImGui-equivalent, fixed 8-segment subdivision).
 *
 * Single source of truth for the immediate selection-chrome outline geometry:
 * the on-screen overlay strokes precisely these polylines. Exposed so tests can
 * assert the flattened geometry without a GL context.
 */
[[nodiscard]] std::vector<OverlayStrokePolyline> OverlayScreenPolylinesForPath(
    const ViewportState& viewport, const Path& pathDoc);

/**
 * Return the indices of `points` where a polyline run must be broken so that an
 * ImGui miter join can never overshoot `miterLimit` half-widths.
 *
 * A joint is a break point when ImGui's actual miter offset there — the
 * `IM_FIXNORMAL2F`-rescaled average of the two adjacent edge normals — exceeds
 * `miterLimit`. Such a run is split so the joint becomes two butt-capped run
 * ends (a bevel), matching what the Donner renderer draws for the same joint
 * under the same miter limit, with no spike. For an open polyline the returned
 * indices are strictly interior and sorted ascending; for a closed polyline any
 * index (including 0) may be returned when its wrap-around joint is sharp.
 *
 * This is the deterministic, GL-free proxy for the visible flare: ImGui's
 * thick-line stroker only caps the miter normal at `IM_FIXNORMAL2F` (inverse
 * length 100), so without these breaks a sharp cusp produces a spur up to ~10
 * half-widths long. Breaking the run there eliminates the spike.
 */
[[nodiscard]] std::vector<std::size_t> OverlayMiterBreakIndices(
    const std::vector<Vector2d>& points, bool closed, double miterLimit = kOverlayStrokeMiterLimit);

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
