#pragma once
/// @file
/// Owns the per-frame decision of where this frame's document pixels land.
///
/// Two presentation targets exist:
///
/// - **Framebuffer underlay** (desktop, and the browser fallback): cached
///   document tiles are composited straight onto the window framebuffer
///   underneath the ImGui UI, together with the WGSL checkerboard backdrop.
/// - **Worker surface** (browser): a worker-owned surface outside the window
///   framebuffer already holds the document pixels. The UI thread only
///   positions that surface from the live viewport and reports the accepted
///   frame token, and falls back to the framebuffer underlay whenever the
///   worker has no accepted surface epoch for this frame.
///
/// Before this seam existed the choice was an `#ifdef` fork inside
/// `EditorShell::renderRenderPanePresentation`. It now collapses into presenter
/// construction: the shell builds exactly one `DocumentPresenter` and drives it
/// through the same two calls on every platform.
///
/// The presenter deliberately owns no rendering resources. Both concrete
/// presenters talk to the outside world through injected sinks, so the
/// placement math and the frame-token gating are unit-testable headlessly on
/// any build.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

/// Which surface owns document pixels for this build.
enum class DocumentPresentationTarget : std::uint8_t {
  /// Document tiles composite into the window framebuffer under the ImGui UI.
  FramebufferUnderlay,
  /// A worker-owned surface outside the window framebuffer holds document
  /// pixels, with the framebuffer underlay kept as the per-frame fallback.
  WorkerSurface,
};

/// The presentation target this build presents through.
[[nodiscard]] constexpr DocumentPresentationTarget DefaultDocumentPresentationTarget() {
#if defined(DONNER_EDITOR_WGPU) && defined(__EMSCRIPTEN__)
  return DocumentPresentationTarget::WorkerSurface;
#else
  return DocumentPresentationTarget::FramebufferUnderlay;
#endif
}

/// Placement for one worker-owned document surface update, in screen pixels.
///
/// Mirrors the browser layout bridge's DOM contract one-for-one: `left`/`top`/
/// `width`/`height` place the surface, the four `clip*` insets carry the
/// pane clip, and `frameToken` is the worker's accepted surface epoch.
struct WorkerSurfaceLayout {
  /// Whether the surface should be shown at all this frame.
  bool visible = false;
  /// Which of the worker's surface slots holds the accepted pixels.
  int surfaceSlot = 0;
  /// Screen-space left edge of the document rect.
  double left = 0.0;
  /// Screen-space top edge of the document rect.
  double top = 0.0;
  /// Screen-space width of the document rect.
  double width = 0.0;
  /// Screen-space height of the document rect.
  double height = 0.0;
  /// Inset from the surface's left edge to the pane-clipped left edge.
  double clipLeft = 0.0;
  /// Inset from the surface's top edge to the pane-clipped top edge.
  double clipTop = 0.0;
  /// Inset from the pane-clipped right edge to the surface's right edge.
  double clipRight = 0.0;
  /// Inset from the pane-clipped bottom edge to the surface's bottom edge.
  double clipBottom = 0.0;
  /// Accepted worker surface epoch represented by these pixels.
  std::uint64_t frameToken = 0;
  /// Whether the accepted pixels already contain Select-mode selection chrome.
  bool selectionChromeBaked = false;
};

/// Sink applying one \ref WorkerSurfaceLayout to the worker-owned surface.
using WorkerSurfaceLayoutSink = std::function<void(const WorkerSurfaceLayout&)>;

/// The layout sink this build presents through: the browser DOM bridge on
/// wasm+WebGPU builds, and a no-op everywhere else.
[[nodiscard]] WorkerSurfaceLayoutSink DefaultWorkerSurfaceLayoutSink();

/// Everything the framebuffer underlay needs to draw one frame of document
/// pixels. Captured by value because the underlay callback runs later in the
/// frame, after the shell's borrowed tile views would have gone stale.
struct FramebufferUnderlayPlan {
  /// Viewport transform the tiles are placed with.
  ViewportState viewport;
  /// Document rect clipped to the render pane, in screen pixels.
  Box2d documentClipRect;
  /// Low-resolution whole-document tiles drawn beneath the active tiles.
  std::vector<GlTextureCache::TileView> overviewTiles;
  /// Viewport-resolution tiles for this frame.
  std::vector<GlTextureCache::TileView> tiles;
  /// Live drag preview whose transform the presented tiles are advanced by.
  std::optional<SelectTool::ActiveDragPreview> activeDragPreview;
  /// Drag preview the currently presented tiles were rasterized against.
  std::optional<SelectTool::ActiveDragPreview> displayedDragPreview;
  /// Layer whose tile must not be drawn (pen live preview, hidden layer).
  Entity suppressedLayerEntity = entt::null;
  /// Whether drag-target tiles are suppressed for this frame.
  bool suppressDragTargetTiles = false;
};

/// Sink installing (or, on `std::nullopt`, clearing) this frame's framebuffer
/// underlay plan.
using FramebufferUnderlayPlanSink = std::function<void(std::optional<FramebufferUnderlayPlan>)>;

/// Per-frame inputs to the external-surface half of the presentation decision.
struct DocumentPresentationFrame {
  /// Live viewport transform for this frame.
  ViewportState viewport;
  /// Render-pane rect in screen pixels; presented pixels are clipped to it.
  Box2d paneRect;
  /// True when this frame must present nothing outside the ImGui draw list,
  /// because a content-only capture or the sample picker owns the pane.
  bool presentationSuppressed = false;
  /// The worker's accepted surface state as of this frame. `active` is false
  /// on builds with no worker surface.
  DirectSurfacePresentationState workerSurface;
};

/// What the presenter resolved for this frame.
struct DocumentPresentationResult {
  /// True when document pixels land outside the ImGui draw list this frame, so
  /// `RenderPanePresenter` must draw neither tiles nor its own checkerboard.
  bool documentPresentedDirectly = false;
  /// True when a surface outside the window framebuffer holds this frame's
  /// document pixels. The framebuffer underlay must then stay cleared, and
  /// selection chrome must reuse the transform baked into those pixels.
  bool externalSurfacePresented = false;
  /// True when the presented external surface already contains selection
  /// chrome, so the overlay pass must drop the baked primitives.
  bool selectionChromeBaked = false;
};

/// Screen placement for a worker-owned document surface this frame, or
/// `std::nullopt` when the surface must stay hidden.
///
/// Pure geometry: the accepted epoch's document rect mapped through the viewport
/// that epoch was rasterized against (falling back to the live viewport when the
/// epoch published none), rejected when it is empty or falls entirely outside the
/// live render pane. Placing an epoch through a later viewport would stretch a
/// viewport-bounded high-zoom raster past the region its pixels cover and uncover
/// the editor background inside the pane.
[[nodiscard]] std::optional<WorkerSurfaceLayout> ComputeWorkerSurfaceLayout(
    const DocumentPresentationFrame& frame);

/**
 * The per-frame document presentation decision.
 *
 * Driven in two ordered stages per frame, because the shell's own work sits
 * between them:
 *
 * 1. \ref resolveExternalSurface runs before selection-chrome rasterization,
 *    which needs to know whether an external surface already owns (and has
 *    already baked chrome into) this frame's pixels.
 * 2. \ref presentUnderlay runs after tile suppression is final.
 *
 * Calling the stages out of order is a programming error; implementations
 * refuse the second stage rather than installing a plan against a frame that
 * was never opened.
 */
class DocumentPresenter {
public:
  virtual ~DocumentPresenter() = default;

  /**
   * Stage 1: resolve whether a surface outside the window framebuffer owns this
   * frame's document pixels, and place that surface if so. Opens the frame.
   *
   * @param frame Per-frame presentation inputs.
   * @return What this frame resolved to: whether document pixels land outside
   *   the ImGui draw list, and whether selection chrome is already baked in.
   */
  virtual DocumentPresentationResult resolveExternalSurface(
      const DocumentPresentationFrame& frame) = 0;

  /**
   * Stage 2: install this frame's framebuffer underlay plan, or clear the
   * underlay when there is nothing directly presentable. Closes the frame.
   *
   * @param plan Tiles and placement for the underlay, or `std::nullopt`.
   * @return True when the underlay presents document pixels this frame.
   */
  virtual bool presentUnderlay(std::optional<FramebufferUnderlayPlan> plan) = 0;

  /// True when document pixels can land on a surface outside the window
  /// framebuffer on this build.
  [[nodiscard]] virtual bool presentsToExternalSurface() const = 0;

  /// Count of stage-2 calls refused because no frame was open. Diagnostic only.
  [[nodiscard]] virtual std::uint64_t refusedUnderlayPresentCount() const = 0;
};

/**
 * Presents document tiles by compositing them onto the window framebuffer
 * underneath the ImGui UI. The desktop presenter, and the browser's fallback
 * whenever the worker has no accepted surface epoch.
 */
class FramebufferUnderlayPresenter final : public DocumentPresenter {
public:
  /**
   * @param planSink Installs or clears the window's underlay draw plan.
   */
  explicit FramebufferUnderlayPresenter(FramebufferUnderlayPlanSink planSink);

  DocumentPresentationResult resolveExternalSurface(
      const DocumentPresentationFrame& frame) override;
  bool presentUnderlay(std::optional<FramebufferUnderlayPlan> plan) override;
  [[nodiscard]] bool presentsToExternalSurface() const override { return false; }
  [[nodiscard]] std::uint64_t refusedUnderlayPresentCount() const override {
    return refusedUnderlayPresents_;
  }

private:
  FramebufferUnderlayPlanSink planSink_;
  bool frameOpen_ = false;
  std::uint64_t refusedUnderlayPresents_ = 0;
};

/**
 * Presents the document through a worker-owned surface outside the window
 * framebuffer, positioning that surface from the live viewport each frame.
 *
 * Wraps a fallback presenter (the framebuffer underlay) that takes over on any
 * frame the worker surface cannot own, so a build has exactly one top-level
 * presenter regardless of which surface wins a given frame.
 */
class WorkerSurfacePresenter final : public DocumentPresenter {
public:
  /**
   * @param layoutSink Applies one surface layout update per frame. Called on
   *   every frame, including frames that hide the surface, so the surface can
   *   never latch a stale placement.
   * @param fallback Presenter used on frames the worker surface cannot own.
   */
  WorkerSurfacePresenter(WorkerSurfaceLayoutSink layoutSink,
                         std::unique_ptr<DocumentPresenter> fallback);

  DocumentPresentationResult resolveExternalSurface(
      const DocumentPresentationFrame& frame) override;
  bool presentUnderlay(std::optional<FramebufferUnderlayPlan> plan) override;
  [[nodiscard]] bool presentsToExternalSurface() const override { return true; }
  [[nodiscard]] std::uint64_t refusedUnderlayPresentCount() const override {
    return refusedUnderlayPresents_;
  }

  /// The layout most recently handed to the sink. Diagnostic/testing only.
  [[nodiscard]] const std::optional<WorkerSurfaceLayout>& lastLayout() const { return lastLayout_; }

  /// The accepted placement held across a document replacement. Diagnostic/testing only.
  [[nodiscard]] const std::optional<WorkerSurfaceLayout>& heldLayout() const { return heldLayout_; }

private:
  WorkerSurfaceLayoutSink layoutSink_;
  std::unique_ptr<DocumentPresenter> fallback_;
  std::optional<WorkerSurfaceLayout> lastLayout_;
  /// Last placement an accepted epoch produced, replayed while a document swap
  /// leaves the worker with no accepted epoch at all.
  std::optional<WorkerSurfaceLayout> heldLayout_;
  /// Frames the hold has covered since the accepted epoch went inactive. The
  /// hold is a bridge across a document swap, not a steady state: if no
  /// replacement epoch arrives within the budget (a parked terminal surface
  /// failure, a wedged worker), drop it so the framebuffer underlay's
  /// checkerboard fallback takes the pane, matching desktop behavior.
  int heldFrameCount_ = 0;
  bool frameOpen_ = false;
  bool surfaceOwnsFrame_ = false;
  std::uint64_t refusedUnderlayPresents_ = 0;
};

/**
 * Build the presenter for one presentation target.
 *
 * @param target Which surface owns document pixels. Production callers pass
 *   \ref DefaultDocumentPresentationTarget(); tests pass either value so both
 *   forks stay covered on one build.
 * @param planSink Installs or clears the framebuffer underlay draw plan.
 * @param layoutSink Applies worker-surface layout updates. Unused when
 *   `target` is `FramebufferUnderlay`.
 */
[[nodiscard]] std::unique_ptr<DocumentPresenter> MakeDocumentPresenter(
    DocumentPresentationTarget target, FramebufferUnderlayPlanSink planSink,
    WorkerSurfaceLayoutSink layoutSink);

}  // namespace donner::editor
