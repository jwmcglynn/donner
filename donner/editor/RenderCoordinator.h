#pragma once
/// @file

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Path.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/AsyncSVGDocument.h"
#include "donner/editor/CompositedPresentation.h"
#include "donner/editor/DocumentPixelSampler.h"
#include "donner/editor/FrameCostBreakdown.h"
#include "donner/editor/FramePresentation.h"
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

/// Identity shared by a worker pixel request and its accepted presentation.
struct DocumentPixelCaptureIdentity {
  std::uint64_t sessionId = 0;               //!< Pixel-capture session that owns this request.
  std::uint64_t documentGeneration = 0;      //!< Document generation represented by the capture.
  std::uint64_t version = 0;                 //!< Document frame version represented by the capture.
  std::uint64_t fontResourceRevision = 0;    //!< Font-resource revision used for the capture.
  std::uint64_t canvasCommitGeneration = 0;  //!< Canvas-size commit represented by this render.
  EditorRasterViewport rasterViewport;       //!< Raster viewport used to produce document pixels.
  ViewportState viewport;                    //!< Viewport transform associated with the capture.
};

/// One immutable document-only pixel capture retained while the picker is armed.
struct DocumentPixelCapture {
  DocumentPixelCaptureIdentity identity;  //!< Request identity required to accept the capture.
  svg::RendererBitmap bitmap;             //!< Document-only pixels, without editor chrome.
};

/// What a posted render request asked the worker for, as far as deciding whether a later request
/// would repeat it.
/// Stable repair inputs, excluding allocation counters that cannot make a retry productive.
struct PresentationRepairIdentity {
  PresentationIdentity scene;
  std::vector<Entity> selection;
  std::vector<PresentationPose> poses;
  EditorRasterViewport coverage;
  FramePresentationFailure failure = FramePresentationFailure::None;
};

struct RenderAttemptIdentity {
  std::uint64_t documentGeneration = 0;  //!< Document the request rendered.
  std::uint64_t version = 0;             //!< Document frame version the request rendered.
  EditorRasterViewport rasterViewport;   //!< Raster the request rendered into.
  bool overviewInfillOnly = false;       //!< True for an overview infill request.
  Entity selectedEntity = entt::null;    //!< Selected entity the request kept promoted.
  std::optional<RenderRequest::DragPreview> dragPreview;  //!< Drag state the request carried.
  std::uint64_t presentationEpoch = 0;  //!< Presentation-refresh epoch for renderer settings.
  std::optional<PresentationRepairIdentity> repair;
};

/**
 * Paces re-posting a render request whose worker result had nothing to present.
 *
 * Such a result (no compositor tile: a failed readback, a lost device, refused surfaces) marks
 * nothing rendered, and every idle frame asks for a render, so without pacing the identical
 * request would be posted again on every frame. The failed request may be posted again after each
 * delay in \ref kRetryDelays; once the last retry fails too, an identical request is held until
 * something about it changes. A different request is never held.
 */
class NothingToPresentRetry {
public:
  /// Steady clock used to pace identical retry attempts.
  using Clock = std::chrono::steady_clock;

  /// Delay before each successive retry of the same failed request.
  static constexpr std::array<std::chrono::milliseconds, 3> kRetryDelays = {
      std::chrono::milliseconds(100), std::chrono::milliseconds(500),
      std::chrono::milliseconds(2000)};

  /**
   * Record that the result of \p attempt had nothing to present.
   *
   * @param attempt The request that produced the result.
   * @param now Current time.
   * @return True when this failure used up the last retry.
   */
  bool noteFailure(const RenderAttemptIdentity& attempt, Clock::time_point now);

  /**
   * Whether \p attempt may be posted at \p now.
   *
   * @param attempt The request about to be posted.
   * @param now Current time.
   */
  [[nodiscard]] bool mayPost(const RenderAttemptIdentity& attempt, Clock::time_point now) const;

  /// True while a retry of the failed request is still scheduled.
  [[nodiscard]] bool retryScheduled() const;

  /**
   * Seconds until the scheduled retry is due, or nullopt when none is scheduled or it is already
   * due: the wake for it has fired, and the next frame that asks for a render posts it.
   *
   * @param now Current time.
   */
  [[nodiscard]] std::optional<float> secondsUntilRetry(Clock::time_point now) const;

  /// Forget the failed request, once a result presents or the document is replaced.
  void reset();

private:
  std::optional<RenderAttemptIdentity> failedAttempt_;
  std::size_t failures_ = 0;
  Clock::time_point retryAt_{};
};

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
 * Return the document version a released drag must settle against.
 *
 * @param currentFrameVersion Last flushed document frame version at mouse-up.
 * @param hasPendingMutations True when the release left DOM mutations queued for the next flush.
 */
[[nodiscard]] std::uint64_t PostReleaseSettleTargetVersion(std::uint64_t currentFrameVersion,
                                                           bool hasPendingMutations);

/**
 * Return true when selected-layer prewarm can wait for the viewport to settle.
 *
 * @param selectedEntity Selected compositable entity, or entt::null.
 * @param hasActiveDrag True while a drag preview is active.
 * @param currentVersion Current document frame version.
 * @param displayedDocVersion Document frame version currently presented.
 * @param hasCachedPresentation True when any current presentation texture is already cached.
 *   The selected layer may still be warming; the existing full presentation is sufficient to
 *   keep zoom responsive while that stale prewarm is cancelled.
 * @param rasterViewportSettled True when the viewport has passed the commit delay.
 * @param needsOverviewInfill True when a bounded viewport still needs overview coverage.
 * @param pendingSelectedLayerRasterization True when selected-layer pixels are known stale.
 */
[[nodiscard]] bool ShouldDeferSelectedViewportRefresh(
    Entity selectedEntity, bool hasActiveDrag, std::uint64_t currentVersion,
    std::uint64_t displayedDocVersion, bool hasCachedPresentation, bool rasterViewportSettled,
    bool needsOverviewInfill, bool pendingSelectedLayerRasterization);

/**
 * Return true when selected-layer overdraw should replace the base raster viewport.
 *
 * Direct worker surfaces disable selection-only prewarming because the UI thread cannot consume a
 * promoted texture. They still use overdraw when a drag or another real invalidation needs a
 * worker render.
 *
 * @param selectedEntity Selected compositable entity, or entt::null.
 * @param requestOverviewInfill True when the request is reserved for full-document overview fill.
 * @param rasterViewportBounded True when the base raster only covers the visible viewport.
 * @param selectionOnlyPrewarmMayTriggerRender Backend policy for idle selection cache misses.
 * @param hasIndependentRenderReason True when drag, document invalidation, forced rasterization, or
 *   retry already requires a worker request.
 * @param hasCompleteVisibleCachedCoverage True when the currently presented tiles already cover
 *   the visible raster at its requested pixel scale. Enlarging the raster in this state would
 *   invalidate every cached static segment before the first pointer move.
 * @param visibleOutputSizePx Size of the visible raster.
 * @param prewarmOutputSizePx Size of the proposed enlarged selection raster.
 */
[[nodiscard]] bool ShouldUseSelectedPrewarmRasterViewport(
    Entity selectedEntity, bool requestOverviewInfill, bool rasterViewportBounded,
    bool selectionOnlyPrewarmMayTriggerRender, bool hasIndependentRenderReason,
    bool hasCompleteVisibleCachedCoverage, Vector2i visibleOutputSizePx,
    Vector2i prewarmOutputSizePx);

/**
 * Return true when a render result satisfies a pending selected-layer rasterization.
 *
 * @param representedDragPreview Drag-preview metadata carried by the completed render result.
 * @param pendingEntity Selected entity whose stale layer pixels must be refreshed.
 * @param resultVersion Document frame version represented by the completed render result.
 * @param pendingVersion Document frame version at which the pending refresh was requested.
 */
[[nodiscard]] bool ShouldClearPendingSelectedLayerRasterization(
    const std::optional<RenderRequest::DragPreview>& representedDragPreview, Entity pendingEntity,
    std::uint64_t resultVersion, std::uint64_t pendingVersion);

/// A complete forced render clears stale selected pixels whether they landed in a movable layer
/// or in the selection's owning compositor tiles.
[[nodiscard]] bool CompositedPreviewClearsPendingSelectedLayerRasterization(
    const RenderResult::CompositedPreview& preview, Entity pendingEntity,
    std::uint64_t resultVersion, std::uint64_t pendingVersion);

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
 * Return the document transform that projects geometry from one drag frame onto another.
 *
 * @param sourceDragPreview Drag transform represented by the source geometry.
 * @param targetDragPreview Drag transform the presented content represents.
 */
[[nodiscard]] Transform2d OverlayDocumentFromSourceDragPreview(
    const std::optional<SelectTool::ActiveDragPreview>& sourceDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& targetDragPreview);

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
  /// Construct rendering and presentation coordination for the selected backend.
  /// @param geodeDevice Shared Geode device, or null for a non-Geode renderer.
  explicit RenderCoordinator(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice = nullptr);

  /// Expose the background render worker.
  [[nodiscard]] AsyncRenderer& asyncRenderer() { return renderWorker_.asyncRenderer; }
  /// Return the background render worker.
  [[nodiscard]] const AsyncRenderer& asyncRenderer() const { return renderWorker_.asyncRenderer; }
  /// Expose the renderer owned by the worker bundle.
  [[nodiscard]] svg::Renderer& renderer() { return renderWorker_.renderer; }
  /// Return cached selection bounds and their document versions.
  [[nodiscard]] const SelectionBoundsCache& selectionBoundsCache() const {
    return selectionBoundsCache_;
  }
  /// Expose composited presentation state for the UI thread.
  [[nodiscard]] CompositedPresentation& compositedPresentation() { return compositedPresentation_; }
  /// Return composited presentation state.
  [[nodiscard]] const CompositedPresentation& compositedPresentation() const {
    return compositedPresentation_;
  }
  /// Return the document version last accepted for visible presentation.
  [[nodiscard]] std::uint64_t displayedDocVersion() const { return displayedDocVersion_; }
  /// Latest editor rendering cost counters observed by this coordinator.
  [[nodiscard]] const FrameCostBreakdown& lastFrameCostBreakdown() const {
    return lastFrameCostBreakdown_;
  }
  /// Document canvas-size commits this coordinator has made since startup.
  ///
  /// Cumulative, unlike the per-frame counter in `FrameCostBreakdown`. A commit
  /// invalidates the render tree, so it forces a document render that no
  /// interaction asked for; the browser suites need to tell that render apart
  /// from one an interaction caused before they can assert on render counts,
  /// and the commit is evaluated inside a rendered frame, so a commit still
  /// pending when the demand-driven loop parks lands on whatever frame the next
  /// interaction wakes.
  [[nodiscard]] std::uint64_t documentCanvasCommitTotal() const {
    return documentCanvasCommitTotal_;
  }
  /// Overview-infill render requests posted since startup.
  ///
  /// An overview-infill request carries no selection prewarm and no drag: it
  /// exists to restore whole-document coverage the presenter is missing, so
  /// like a canvas-size commit it is a render the editor owes itself rather
  /// than one an interaction asked for. Whether the previous coverage survives
  /// an interaction is cache- and timing-dependent, which is why a suite that
  /// counts an interaction's renders has to be able to name this one.
  [[nodiscard]] std::uint64_t overviewInfillRenderTotal() const {
    return overviewInfillRenderTotal_;
  }
  /// Request one worker render even when document and viewport epochs are already current. The
  /// request is a new one even if an identical earlier request is being held back.
  void requestPresentationRefresh() {
    pendingPresentationRefresh_ = true;
    ++presentationEpoch_;
    pendingOverviewResult_.reset();
    overviewDocVersion_ = 0;
  }
  /// Arm or disarm the worker-owned document pixel capture.
  void setDocumentPixelCaptureEnabled(bool enabled);
  /// Whether an editor session currently retains or requests document pixels.
  [[nodiscard]] bool documentPixelCaptureEnabled() const { return documentPixelCaptureEnabled_; }
  /// Current capture, or null when the worker result has not matched the live presentation.
  [[nodiscard]] const DocumentPixelCapture* documentPixelCaptureFor(
      const EditorApp& app, const ViewportState& viewport) const;
  /// True when the current capture attempt completed without a usable bitmap.
  [[nodiscard]] bool documentPixelCaptureUnavailable() const { return captureUnavailable_; }
  /// Wake deadline for an armed picker waiting on a delayed semantic canvas-size commit.
  [[nodiscard]] std::optional<float> nextPixelCaptureCanvasCommitWakeSeconds() const;
  /// Wake deadline for the paced re-render of a request whose result had nothing to present.
  [[nodiscard]] std::optional<float> nextNothingToPresentRetryWakeSeconds() const;
  /// Whether a renderer-presentation setting still needs a worker frame.
  [[nodiscard]] bool presentationRefreshPending() const { return pendingPresentationRefresh_; }
  /// Request a missing coherent frame without invalidating already-running work.
  [[nodiscard]] bool frameRepairPending() const { return presentationNeedsRender_; }
  /// Whether the sealed frame represents the input considered by the latest UI frame.
  [[nodiscard]] bool frameRepresentsCurrentIntent() const { return frameRepresentsCurrentIntent_; }

  /// Clear the per-frame cost accumulator before a new UI frame starts.
  void beginFrameCostTracking() { lastFrameCostBreakdown_ = FrameCostBreakdown{}; }
  /// Replace transient source-hover chrome elements.
  ///
  /// @param elements Elements to highlight as source-hover preview chrome.
  /// @return true if the hover preview changed.
  bool setSourceHoverElements(std::vector<svg::SVGElement> elements);

  /// Update the transient locked-rejection flash to draw on the next overlay capture. The flash
  /// fades as `SelectTool` ticks it; this should be pushed once per frame from the editor's
  /// per-frame path with `selectTool.lockedRejectionFlash()`.
  ///
  /// @param flash Active flash (element + intensity), or nullopt when no element is being rejected.
  void setLockedRejectionFlash(std::optional<SelectTool::LockedRejectionFlash> flash);

  /// @return true when a locked-rejection flash is currently stored (still fading).
  [[nodiscard]] bool hasLockedRejectionFlash() const {
    return lockedRejectionFlash_.has_value() && lockedRejectionFlash_->intensity > 0.0f;
  }

  /// Set (or clear) the path element the Pen tool is actively editing. While
  /// set, every overlay capture also snapshots the element's live
  /// document-space geometry + solid paint into
  /// `SelectionChromeSnapshot::livePathPreview`, so the presented frame shows
  /// the edited path from the same DOM capture as the chrome instead of the
  /// stale async raster.
  void setPenLivePreviewElement(std::optional<svg::SVGElement> element) {
    penLivePreviewElement_ = std::move(element);
  }
  /// @return the active pen live-preview element, if any.
  [[nodiscard]] const std::optional<svg::SVGElement>& penLivePreviewElement() const {
    return penLivePreviewElement_;
  }

  /// Set (or clear) the Pen tool's hover chrome for the next overlay capture:
  /// the rubber-band preview of the segment a click would commit, and the
  /// close-path affordance point when the pointer is within closing range.
  void setPenHoverChrome(std::optional<Path> previewSegmentDoc,
                         std::optional<Vector2d> closeAffordanceDoc) {
    penHoverPreviewSegmentDoc_ = std::move(previewSegmentDoc);
    penHoverCloseAffordanceDoc_ = closeAffordanceDoc;
  }

  /// Retain a text annotation together with the exact layout state used to measure it.
  /// @param text Captured text geometry, or null to end the editing annotation.
  void setTextEditingChrome(std::optional<CapturedPresentation::TextEditing> text) {
    if (text.has_value()) {
      text->identity.presentationEpoch = presentationEpoch_;
    }
    textEditing_ = std::move(text);
  }

  /// Set (or clear) the text tool's drag-to-create preview chrome for the
  /// next overlay capture: the live box being dragged out plus its first
  /// baseline and I-beam marker, in document space.
  void setTextBoxDragPreview(
      std::optional<SelectionChromeSnapshot::TextBoxDragPreview> previewDoc) {
    textBoxDragPreviewDoc_ = previewDoc;
  }

  /**
   * Reset presentation state after loading a different document.
   *
   * @param documentGeneration Generation of the newly loaded document.
   */
  void resetForLoadedDocument(std::uint64_t documentGeneration);
  /// Refresh selected-element bounds from the live document, or clear them if no document exists.
  /// @param app Editor application holding the current selection.
  void refreshSelectionBoundsCache(EditorApp& app);
  /// Promote pending selection bounds once their document version is displayed.
  void promoteSelectionBoundsIfReady();
  /// Request annotation geometry from the next safe state matching the accepted raster.
  void requestSelectionGeometryRefresh() { selectionGeometryRefreshRequested_ = true; }
  /// Drain the latest async-render result into the editor's UI state.
  /// If a `frameHistory` is supplied, its latest slot is stamped with
  /// the backend (worker) ms reported by `AsyncRenderer` so the frame
  /// graph can plot backend time alongside ImGui frame time. `nullptr`
  /// is allowed for callers that don't care about backend timing.
  void pollRenderResult(EditorApp& app, const ViewportState& viewport, GlTextureCache& textures,
                        FrameHistory* frameHistory = nullptr);
  /// Post a worker render when document, viewport, and presentation state require one.
  /// @param app Editor application and current document.
  /// @param selectTool Active drag and selection preview source.
  /// @param viewport Desired canvas and raster viewport.
  /// @param textures Optional cache for viewport-coverage diagnostics.
  /// @param supersedeInFlight Whether an in-flight worker render may be replaced.
  /// @param directSurfaceSelectionDetail Reserved; scheduling currently ignores this value.
  /// @return True only when a new worker request was posted.
  bool maybeRequestRender(
      EditorApp& app, SelectTool& selectTool, const ViewportState& viewport,
      GlTextureCache* textures = nullptr, bool supersedeInFlight = false,
      SelectionChromeDetail directSurfaceSelectionDetail = SelectionChromeDetail::Full);
  /// Record document-flush invalidation that depends on the live selected element.
  ///
  /// @param app Editor application state containing the live selection.
  /// @param flushResult Metadata from the just-flushed editor command batch.
  void invalidatePresentationAfterDocumentFlush(EditorApp& app,
                                                const AsyncSVGDocument::FlushResult& flushResult);
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
  /// Seal the resource, object-pose and selection decision used by every draw pass this frame.
  std::shared_ptr<const FramePresentation> buildFramePresentation(EditorApp& app, SelectTool& tool,
                                                                  const ViewportState& viewport,
                                                                  const Box2d& paneClipRect,
                                                                  SelectionChromeDetail detail,
                                                                  bool includeChrome = true);
  /// Last installed immutable frame, retained for diagnostics and render callbacks.
  [[nodiscard]] std::shared_ptr<const FramePresentation> framePresentation() const {
    return framePresentation_;
  }
  /// Latest race-free overlay chrome snapshot for immediate screen-space presentation.
  [[nodiscard]] const std::optional<SelectionChromeSnapshot>& immediateOverlaySnapshot() const {
    return immediateOverlaySnapshot_;
  }
  /// Document version represented by \ref immediateOverlaySnapshot, if one is currently cached.
  [[nodiscard]] std::optional<std::uint64_t> immediateOverlayDocumentVersionForDiagnostics() const {
    return framePresentation_
               ? std::optional<std::uint64_t>(framePresentation_->selectionIdentity().version)
               : std::nullopt;
  }
  /// Pending selected-layer rasterization entity for replay diagnostics.
  [[nodiscard]] Entity pendingSelectedLayerRasterizationEntityForDiagnostics() const {
    return pendingSelectedLayerRasterizationEntity_;
  }
  /// Pending selected-layer rasterization document version for replay diagnostics.
  [[nodiscard]] std::uint64_t pendingSelectedLayerRasterizationVersionForDiagnostics() const {
    return pendingSelectedLayerRasterizationVersion_;
  }
  /// Last document version published to the visible presentation for replay diagnostics.
  [[nodiscard]] std::uint64_t displayedDocVersionForDiagnostics() const {
    return displayedDocVersion_;
  }
  /// Cumulative count of worker results that had nothing to present. Diagnostics only: a count
  /// that keeps rising under a frozen canvas names a renderer that produces nothing, not a stalled
  /// worker.
  [[nodiscard]] std::uint64_t nothingToPresentResultTotalForDiagnostics() const {
    return nothingToPresentResultTotal_;
  }
  /// Selected entity eligible for composited presentation for replay diagnostics.
  [[nodiscard]] Entity selectedCompositedEntityForDiagnostics(EditorApp& app) const;

private:
  std::vector<Entity> selectedPresentationEntities(const EditorApp& app) const;
  std::optional<LockedRejectionFlashInput> lockedFlashForCapture() const;
  bool shouldRecaptureSelection(const CapturedPresentation& captured,
                                const std::vector<Entity>& selection) const;
  void refreshFrameSelectionCapture(EditorApp& app,
                                    const std::shared_ptr<const CapturedPresentation>& captured,
                                    const std::vector<Entity>& selection);
  FramePresentationInput makeFrameInput(EditorApp& app, SelectTool& tool,
                                        const ViewportState& viewport, const Box2d& paneClipRect,
                                        SelectionChromeDetail detail, bool includeChrome);
  void refreshLivePathCapture(EditorApp& app, FramePresentationInput& input);
  void recordFramePresentationCost(const FramePresentationInput& input, const SelectTool& tool,
                                   double elapsedMs);
  bool hasForcedRenderReason(bool captureNeeded) const;
  bool prepareResultResources(RenderResult& result, EditorApp& app, GlTextureCache& textures);
  std::optional<SelectTool::ActiveDragPreview> previewWithPresentationEpoch(
      std::optional<SelectTool::ActiveDragPreview> preview) const;
  void capturePresentationRequest(RenderRequest& request, const EditorApp& app) const;

  friend class EditorShellTestAccess;
  friend struct RenderCoordinatorTestAccess;
  void noteMissingPixelCaptureResult(const std::optional<RenderResult>& result);
  void rejectRenderResult(const std::optional<RenderResult>& result);
  void rejectPixelCaptureResult(const std::optional<RenderResult>& result);
  void noteResultWithNothingToPresent(const std::optional<RenderResult>& result);
  void noteSelectedPrewarmResultPresented(const RenderResult& result);
  bool selectedPrewarmFallbackApplies(std::uint64_t documentGeneration, Entity selectedEntity,
                                      const EditorRasterViewport& visibleRaster);
  [[nodiscard]] bool shouldRequestSelectionOnlyPrewarm(
      std::uint64_t documentGeneration, Entity selectedEntity, std::uint64_t version,
      const EditorRasterViewport& visibleRaster) const;
  void noteSelectedPromotionAvailability(const RenderResult& result);
  [[nodiscard]] std::chrono::steady_clock::time_point nothingToPresentRetryNow() const;
  void acceptPixelCaptureResult(RenderResult& result, const EditorApp& app,
                                const ViewportState& viewport);
  [[nodiscard]] bool preparePixelCaptureRequest(const EditorApp& app, const ViewportState& viewport,
                                                DocumentPixelCaptureIdentity* desired);
  [[nodiscard]] bool pixelCaptureBlocksViewportDefer(bool captureNeeded) const;
  [[nodiscard]] bool shouldDeferViewportRender(bool selectedViewportDeferred,
                                               bool needsOverviewInfill, bool captureNeeded) const;
  [[nodiscard]] bool shouldCapturePixelsForRequest(
      bool requestOverviewInfill, bool activeDrag,
      const EditorRasterViewport& rasterViewport) const;
  void configurePixelCaptureRequest(RenderRequest* request, bool requestOverviewInfill,
                                    bool activeDrag,
                                    const EditorRasterViewport& rasterViewport) const;
  void recordPixelCaptureRequest(const RenderRequest& request,
                                 const DocumentPixelCaptureIdentity& desired);
  void noteCanvasSizeCommitForPixelCapture(const EditorApp& app, const ViewportState& viewport,
                                           DocumentPixelCaptureIdentity* desired,
                                           bool* captureNeeded, bool* forcePresentationRefresh);
  void updatePixelCaptureCanvasCommitWake(bool wouldChange, bool firstCommit,
                                          bool deferForActiveDrag);
  [[nodiscard]] Entity selectedCompositedEntity(EditorApp& app) const;
  [[nodiscard]] bool activeDragNeedsRenderedPresentation(
      EditorApp& app, const std::optional<SelectTool::ActiveDragPreview>& dragPreview,
      const GlTextureCache* textures, Entity suppressedLayerEntity) const;
  [[nodiscard]] std::vector<Entity> selectedCompositedExtraEntities(EditorApp& app,
                                                                    Entity primaryEntity) const;
  Entity suppressedLayerWithoutSelection(EditorApp& app);
  Entity suppressedLayerForHiddenSelection(const svg::SVGElement& selected);

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
  std::shared_ptr<const FramePresentation> framePresentation_;
  std::shared_ptr<const CapturedPresentation> selectedSceneCapture_;
  std::shared_ptr<const CapturedPresentation> livePathCapture_;
  std::uint64_t nextPresentationFrameId_ = 1;
  bool presentationNeedsRender_ = false;
  bool frameRepresentsCurrentIntent_ = false;
  bool presentationNeedsCoverage_ = false;
  FramePresentationFailure adoptionFailure_ = FramePresentationFailure::None;
  std::optional<PresentationRepairIdentity> pendingRepair_;
  std::optional<FramePresentationInput> reusableFrameInput_;
  std::shared_ptr<const GlTextureCache::PresentationResources> reusableFrameResources_;
  std::shared_ptr<const CapturedPresentation> reusableSelectionCapture_;
  void updateFrameRepairStatus(
      const FramePresentationInput& input,
      const std::shared_ptr<const GlTextureCache::PresentationResources>& resources,
      const FramePresentationBuildResult& outcome);
  void installFrameDecision(
      const FramePresentationInput& input,
      const std::shared_ptr<const GlTextureCache::PresentationResources>& resources,
      std::shared_ptr<const FramePresentation> frame);
  void configurePresentationRepair(RenderRequest& request) const;
  bool canReuseFrame(
      const FramePresentationInput& input,
      const std::shared_ptr<const GlTextureCache::PresentationResources>& resources) const;
  std::optional<RenderAttemptIdentity> acceptedRepairAttempt_;
  std::uint64_t lastRejectedRepairCapture_ = 0;
  PresentationIdentity currentPresentationIdentity(const EditorApp& app) const;
  void rejectPreparedPresentation(FramePresentationFailure failure);

  bool selectionGeometryRefreshRequested_ = true;

  std::uint64_t displayedDocVersion_ = 0;

  std::vector<svg::SVGElement> sourceHoverElements_;
  std::optional<SelectTool::LockedRejectionFlash> lockedRejectionFlash_;
  std::optional<svg::SVGElement> penLivePreviewElement_;
  std::optional<Path> penHoverPreviewSegmentDoc_;
  std::optional<Vector2d> penHoverCloseAffordanceDoc_;
  std::optional<CapturedPresentation::TextEditing> textEditing_;
  std::optional<SelectionChromeSnapshot::TextBoxDragPreview> textBoxDragPreviewDoc_;

  void presentCompositedResult(RenderResult& result, EditorApp& app, const ViewportState& viewport,
                               GlTextureCache& textures);
  /// Hold a refreshed overview until detailed tiles can publish the same document version.
  bool canReplaceWithOverview(const RenderResult& result, const EditorApp& app) const;
  void acceptOverviewResult(RenderResult result, EditorApp& app, GlTextureCache& textures);
  /// Whether the staged overview and detailed result describe the current document.
  bool hasMatchingPendingOverview(const RenderResult& result, EditorApp& app) const;
  /// Whether a detailed result has coherent full-document coverage behind it.
  bool canPresentWithOverview(const RenderResult& result,
                              const EditorRasterViewport& rasterViewport, EditorApp& app,
                              const GlTextureCache& textures) const;
  /// Consume stale staging and determine whether full-document coverage needs refreshing.
  bool requiresFreshOverview(bool available, std::uint64_t currentVersion) const;
  void discardStalePendingOverview(const EditorApp& app);
  bool needsOverviewInfillForViewport(EditorApp& app, const EditorRasterViewport& rasterViewport,
                                      bool activeDrag, const GlTextureCache* textures);

  PresentationRenderScheduler renderScheduler_;
  /// Live selected display:none entity whose stale promoted layer is currently hidden.
  Entity displayNoneSuppressedSelectionEntity_ = entt::null;
  /// Cached promoted layer entity hidden for \ref displayNoneSuppressedSelectionEntity_.
  Entity displayNoneSuppressedLayerEntity_ = entt::null;
  /// Selected layer whose cached pixels must be re-rasterized on the next landed prewarm.
  Entity pendingSelectedLayerRasterizationEntity_ = entt::null;
  /// Document version that dirtied \ref pendingSelectedLayerRasterizationEntity_.
  std::uint64_t pendingSelectedLayerRasterizationVersion_ = 0;
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
  /// Document version represented by the published full-document overview.
  std::uint64_t overviewDocVersion_ = 0;
  /// A refreshed overview waiting for matching detailed tiles before either becomes visible.
  std::optional<RenderResult> pendingOverviewResult_;
  /// Renderer-only state changed and must be represented by the next accepted worker frame.
  bool pendingPresentationRefresh_ = false;
  /// Count of presentation refreshes requested, carried by each posted request's identity.
  std::uint64_t presentationEpoch_ = 0;
  /// The last request posted to the worker. A result with nothing to present belongs to it.
  std::optional<RenderAttemptIdentity> lastPostedAttempt_;
  struct SelectedPrewarmFallback {
    std::uint64_t documentGeneration = 0;
    Entity selectedEntity = entt::null;
    EditorRasterViewport visibleRaster;
  };
  /// An expanded selected prewarm that exhausted the tile budget; use the visible raster for
  /// this selection until its document or viewport identity changes.
  std::optional<SelectedPrewarmFallback> selectedPrewarmFallback_;
  bool selectedPrewarmRecoveryPending_ = false;
  struct UnavailableSelectedPromotion {
    std::uint64_t documentGeneration = 0;
    std::uint64_t version = 0;
    Entity selectedEntity = entt::null;
    EditorRasterViewport visibleRaster;
  };
  /// A complete owning-tile result for a selection whose interaction tile was refused. Avoid
  /// posting the identical selection-only prewarm every idle frame; retry on any identity change.
  std::optional<UnavailableSelectedPromotion> unavailableSelectedPromotion_;
  /// Paces re-posting a request whose result had nothing to present.
  NothingToPresentRetry nothingToPresentRetry_;
  /// Cumulative count of worker results that had nothing to present.
  std::uint64_t nothingToPresentResultTotal_ = 0;
  /// Test-only replacement for the steady clock that paces \ref nothingToPresentRetry_.
  std::chrono::steady_clock::time_point (*nothingToPresentRetryClockForTesting_)() = nullptr;
  bool documentPixelCaptureEnabled_ = false;
  bool captureUnavailable_ = false;
  std::uint64_t documentPixelCaptureSessionId_ = 0;
  std::optional<DocumentPixelCaptureIdentity> requestedPixelCapture_;
  std::optional<DocumentPixelCapture> documentPixelCapture_;
  std::optional<std::chrono::steady_clock::time_point> pixelCaptureCanvasCommitDue_;
  FrameCostBreakdown lastFrameCostBreakdown_;
  /// Cumulative canvas-size commits; see `documentCanvasCommitTotal`.
  std::uint64_t documentCanvasCommitTotal_ = 0;
  /// Cumulative overview-infill requests; see `overviewInfillRenderTotal`.
  std::uint64_t overviewInfillRenderTotal_ = 0;
#ifdef __EMSCRIPTEN__
  /// Generation of the last worker GPU-wait failure reported to the page.
  /// Device loss is sticky, so publishing is edge-triggered on this rather
  /// than repeated every frame for the rest of the session.
  std::uint64_t publishedGpuWaitGeneration_ = 0;
#endif
};

}  // namespace donner::editor
