#pragma once
/// @file

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/AsyncSVGDocument.h"
#include "donner/editor/CompositedPresentation.h"
#include "donner/editor/FrameCostBreakdown.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentationRenderScheduler.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::geode {
class GeodeDevice;
}

namespace donner::editor {

class SelectTool;

/**
 * Return true when a composited preview may be presented against the current viewport.
 *
 * Full-canvas previews may stretch across transient canvas-size changes. Split composited previews
 * carry per-tile raster bounds, so presenting them against a different canvas epoch can flash stale
 * high-resolution tiles in the wrong place during zoom/drag races.
 *
 * @param preview Worker-produced composited preview.
 * @param viewportDesiredCanvas Canvas size implied by the current UI-frame viewport.
 */
[[nodiscard]] bool ShouldPresentCompositedPreviewForViewport(
    const RenderResult::CompositedPreview& preview, const Vector2i& viewportDesiredCanvas);

/**
 * Return the drag transform that overlay chrome should represent in the current presentation frame.
 *
 * @param activeDragPreview Active drag transform used by the presenter when a drag target tile is
 *   available.
 * @param displayedDragPreview Drag transform represented by the currently cached content.
 * @param hasPresentableActiveDragTarget True when the presenter can draw a matching drag target
 *   tile this frame.
 */
[[nodiscard]] std::optional<SelectTool::ActiveDragPreview>
OverlayRepresentedDragPreviewForPresentation(
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview,
    bool hasPresentableActiveDragTarget);

/**
 * Return the document transform that projects live drag chrome onto the represented presentation.
 *
 * @param liveDragPreview Live drag transform currently applied to the DOM.
 * @param representedDragPreview Drag transform the presented content represents.
 */
[[nodiscard]] Transform2d OverlayRepresentedDocumentFromLiveDocument(
    const std::optional<SelectTool::ActiveDragPreview>& liveDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& representedDragPreview);

/**
 * Project live gesture chrome, including the selection size/angle chip, to the overlay state.
 *
 * @param activeGesturePreview Live gesture preview from the select tool.
 * @param liveDragPreview Live drag transform currently applied to the DOM.
 * @param representedDragPreview Drag transform the overlay/content presentation represents.
 */
[[nodiscard]] std::optional<SelectTool::ActiveGesturePreview> OverlayGesturePreviewForPresentation(
    const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview,
    const std::optional<SelectTool::ActiveDragPreview>& liveDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& representedDragPreview);

/// Owns the advanced editor's renderer-side orchestration: async rendering, overlay rasterization,
/// composited drag presentation, and selection-bounds cache promotion.
class RenderCoordinator {
public:
  explicit RenderCoordinator(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice = nullptr);

  [[nodiscard]] AsyncRenderer& asyncRenderer() { return renderWorker_.asyncRenderer; }
  [[nodiscard]] const AsyncRenderer& asyncRenderer() const { return renderWorker_.asyncRenderer; }
  [[nodiscard]] svg::Renderer& renderer() { return renderWorker_.renderer; }
  [[nodiscard]] const SelectionBoundsCache& selectionBoundsCache() const {
    return selectionBoundsCache_;
  }
  [[nodiscard]] CompositedPresentation& compositedPresentation() { return compositedPresentation_; }
  [[nodiscard]] const CompositedPresentation& compositedPresentation() const {
    return compositedPresentation_;
  }
  [[nodiscard]] std::uint64_t displayedDocVersion() const { return displayedDocVersion_; }
  /// Latest editor rendering cost counters observed by this coordinator.
  [[nodiscard]] const FrameCostBreakdown& lastFrameCostBreakdown() const {
    return lastFrameCostBreakdown_;
  }
  /// Clear the per-frame cost accumulator before a new UI frame starts.
  void beginFrameCostTracking() { lastFrameCostBreakdown_ = FrameCostBreakdown{}; }
  /// Replace transient source-hover chrome elements.
  ///
  /// @param elements Elements to highlight as source-hover preview chrome.
  /// @return true if the hover preview changed.
  bool setSourceHoverElements(std::vector<svg::SVGElement> elements);

  void resetForLoadedDocument();
  void refreshSelectionBoundsCache(EditorApp& app);
  void promoteSelectionBoundsIfReady();
  /// Capture the editor chrome (path outlines, selection AABBs, marquee) for immediate
  /// presentation. `marqueeRectDoc` is the active marquee rectangle in document space (nullopt when
  /// the user isn't marquee-dragging). The snapshot is drawn directly by Donner's OverlayRenderer
  /// straight onto the Geode framebuffer, so selected chrome does not allocate, rasterize,
  /// snapshot, or upload an overlay texture.
  bool rasterizeOverlayForCurrentSelection(
      EditorApp& app, const ViewportState& viewport, const std::optional<Box2d>& marqueeRectDoc,
      std::optional<SelectTool::ActiveDragPreview> representedDragPreview = std::nullopt,
      std::optional<SelectTool::ActiveTransformBoundsPreview> activeBoundsPreview = std::nullopt,
      std::optional<SelectionChromeDetail> selectionDetail = std::nullopt,
      std::optional<SelectTool::ActiveDragPreview> liveDragPreview = std::nullopt);
  /// Rasterize the current UI-frame overlay immediately before presentation.
  ///
  /// Unlike content render scheduling, overlay chrome is intentionally not gated on worker
  /// availability: drag, zoom, and marquee feedback must track the viewport used by the presented
  /// frame.
  bool rasterizeOverlayForPresentation(
      EditorApp& app, SelectTool& selectTool, const ViewportState& viewport,
      GlTextureCache& textures, std::optional<SelectTool::ActiveDragPreview> activeDragPreview,
      std::optional<SelectTool::ActiveDragPreview> representedDragPreview);
  /// Drain the latest async-render result into the editor's UI state.
  /// If a `frameHistory` is supplied, its latest slot is stamped with
  /// the backend (worker) ms reported by `AsyncRenderer` so the frame
  /// graph can plot backend time alongside ImGui frame time. `nullptr`
  /// is allowed for callers that don't care about backend timing.
  void pollRenderResult(EditorApp& app, const ViewportState& viewport, GlTextureCache& textures,
                        FrameHistory* frameHistory = nullptr);
  void maybeRequestRender(EditorApp& app, SelectTool& selectTool, const ViewportState& viewport,
                          GlTextureCache* textures = nullptr);
  /// Record that a document mutation requires a current-version presentation handoff.
  ///
  /// @param flushResult Metadata from the just-flushed editor command batch.
  void invalidatePresentationAfterDocumentFlush(const AsyncSVGDocument::FlushResult& flushResult);
  /**
   * Return the selected layer whose cached pixels should be hidden while editor chrome remains
   * visible. This is used when the live selected element is `display:none`: hit-testing and the
   * next render already treat it as non-rendering, so the presenter must not keep drawing a stale
   * promoted texture for that entity. If a source reparse remapped the selected element to a new
   * entity, this returns the currently cached pre-reparse entity so that stale texture is hidden.
   * Deleted selected layers are also suppressed while the next render catches up so detached cached
   * tiles cannot remain visible.
   *
   * @param app Editor application state containing the live selection.
   * @return Entity whose cached layer should be suppressed, or entt::null if no suppression is
   *   needed.
   */
  [[nodiscard]] Entity suppressedCompositedLayerEntity(EditorApp& app);
  /// Return true when the live selected graphics element is hidden by `display:none`.
  [[nodiscard]] bool selectedElementIsDisplayNone(EditorApp& app) const;
  /// Latest race-free overlay chrome snapshot for immediate screen-space presentation.
  [[nodiscard]] const std::optional<SelectionChromeSnapshot>& immediateOverlaySnapshot() const {
    return immediateOverlaySnapshot_;
  }

private:
  [[nodiscard]] Entity selectedCompositedEntity(EditorApp& app) const;
  [[nodiscard]] std::vector<Entity> selectedCompositedExtraEntities(EditorApp& app,
                                                                    Entity primaryEntity) const;

  struct RenderWorkerBundle {
    explicit RenderWorkerBundle(
        std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice = nullptr);

    // Members are destroyed in reverse declaration order. The async worker
    // joins before the renderer it references is destroyed.
    svg::Renderer renderer;
    AsyncRenderer asyncRenderer;
  };

  RenderWorkerBundle renderWorker_;
  CompositedPresentation compositedPresentation_;
  SelectionBoundsCache selectionBoundsCache_;
  std::optional<SelectionChromeSnapshot> immediateOverlaySnapshot_;

  std::uint64_t displayedDocVersion_ = 0;

  std::vector<svg::SVGElement> lastOverlaySelectionVec_;
  std::vector<svg::SVGElement> sourceHoverElements_;
  std::vector<svg::SVGElement> lastOverlaySourceHoverVec_;
  Vector2i lastOverlayRasterSize_ = Vector2i::Zero();
  std::optional<Box2d> lastOverlayScreenRect_;
  std::optional<Transform2d> lastOverlayCanvasFromDocument_;
  SelectionChromeDetail lastOverlaySelectionDetail_ = SelectionChromeDetail::Full;
  bool lastOverlayInteractionActive_ = false;
  std::chrono::steady_clock::time_point overlayStableSince_{};
  std::uint64_t lastOverlayVersion_ = std::numeric_limits<std::uint64_t>::max();
  /// Last marquee rect captured into the immediate overlay snapshot, or nullopt if the last
  /// overlay capture didn't include one. Used to invalidate cached chrome when marquee geometry
  /// changes.
  std::optional<Box2d> lastOverlayMarqueeRectDoc_;
  /// Active rotation bounds captured into the immediate overlay snapshot, or nullopt when the last
  /// overlay used normal axis-aligned selection bounds.
  std::optional<SelectTool::ActiveTransformBoundsPreview> lastOverlayActiveBoundsPreview_;

  PresentationRenderScheduler renderScheduler_;
  /// Live selected display:none entity whose stale promoted layer is currently hidden.
  Entity displayNoneSuppressedSelectionEntity_ = entt::null;
  /// Cached promoted layer entity hidden for \ref displayNoneSuppressedSelectionEntity_.
  Entity displayNoneSuppressedLayerEntity_ = entt::null;
  /// Most recent desired canvas size requested by `maybeRequestRender`.
  /// Used to debounce continuous pinch-zoom before committing through
  /// `SVGDocument::setCanvasSize`.
  Vector2i pendingCanvasSize_ = Vector2i::Zero();
  std::chrono::steady_clock::time_point pendingCanvasSizeSince_{};
  /// Most recent raster viewport requested by `maybeRequestRender`.
  /// Used to debounce high-zoom viewport refreshes while the presenter
  /// transforms already-cached composited textures during live zoom/pan.
  std::optional<EditorRasterViewport> pendingRasterViewport_;
  std::chrono::steady_clock::time_point pendingRasterViewportSince_{};
  /// True after a structural mutation whose existing overview/full-document cache may contain
  /// deleted pixels. The old presentation remains visible until the replacement render lands.
  bool pendingDocumentMutationOverviewRefresh_ = false;
  FrameCostBreakdown lastFrameCostBreakdown_;
};

}  // namespace donner::editor
