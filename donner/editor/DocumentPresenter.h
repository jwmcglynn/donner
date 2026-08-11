#pragma once
/// @file
/// Owns the per-frame decision of where this frame's document pixels land.
///
/// There is exactly one presentation target on every platform: the
/// **framebuffer underlay**. Cached document tiles are composited straight onto
/// the window framebuffer underneath the ImGui UI, together with the WGSL
/// checkerboard backdrop.
///
/// This used to be a fork. The browser presented through a worker-owned canvas
/// outside the window framebuffer, placed and clipped with CSS from the live
/// viewport, and fell back to the underlay whenever the worker had no accepted
/// surface epoch. Design 0064 deleted that target: the browser build now runs
/// the whole application on one worker thread that owns the single canvas, so
/// the document composites under the UI in the same WebGPU frame that draws the
/// UI, exactly as on desktop. The fork it replaced is the reason this seam
/// exists at all, and the seam is kept because the underlay plan is still worth
/// testing headlessly.
///
/// The presenter deliberately owns no rendering resources. It talks to the
/// outside world through an injected sink, so the placement math is
/// unit-testable headlessly on any build.

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
  /// The viewport this frame's presented document pixels are placed with.
  ///
  /// Equal to the live viewport whenever the framebuffer underlay presents, and
  /// to the accepted epoch's own viewport when a worker surface does. Every
  /// caller that draws *onto* the document - selection chrome, the compositor
  /// tile overlay, the presented image clip - must use this rather than the
  /// live viewport, or gesture feedback separates from the pixels it annotates
  /// by however far the live viewport has run ahead of the worker.
  ViewportState presentedViewport;
};

/**
 * Return whether two viewports place document pixels differently on screen.
 *
 * Compares only what moves the document: the screen/document mapping, the
 * artboard rect, and the device pixel ratio the chrome is rasterized at. Pane
 * origin and size are deliberately excluded - the pane moving does not by
 * itself move the document within it.
 *
 * @param before Viewport the presented pixels were previously placed with.
 * @param after Viewport they are placed with now.
 */
[[nodiscard]] bool DocumentPresentationMappingChanged(const ViewportState& before,
                                                      const ViewportState& after);

/**
 * Return whether re-mapping an accepted epoch through the live viewport still
 * covers every pane pixel the epoch's own placement covered.
 *
 * This is the clamp that lets zoom motion follow the fingers. The accepted
 * pixels are a fixed document rect, so mapping that rect through the live
 * transform is geometrically correct for an unbounded raster - but a
 * viewport-bounded high-zoom raster only covers the pane plus
 * `ViewportState::kHighZoomRasterMarginScreenPx`, and re-mapping it through a
 * zoomed-out live viewport shrinks it inside the pane, uncovering the editor
 * background where document pixels used to be.
 *
 * Only pane area is compared: an epoch's overhang outside the render pane was
 * clipped away and was never on screen to lose. An epoch that covered no pane
 * area at all trivially loses nothing.
 *
 * @param paneRect Render-pane rect in screen pixels.
 * @param epochSurfaceRect Surface rect placed through the epoch's own viewport.
 * @param liveSurfaceRect The same surface rect placed through the live viewport.
 */
[[nodiscard]] bool LivePlacementCoversEpochCoverage(const Box2d& paneRect,
                                                    const Box2d& epochSurfaceRect,
                                                    const Box2d& liveSurfaceRect);

/**
 * The viewport an accepted worker-surface epoch's pixels are placed with.
 *
 * The live viewport runs one or more worker frames ahead during a gesture, so
 * this picks between it and the epoch's own transform:
 *
 * - No usable epoch transform (a worker fallback, or an epoch published before
 *   the field existed): the live viewport, the pre-existing behavior.
 * - A view-box or DPR change: the epoch's transform. The document itself was
 *   re-framed, so the raster's document rect no longer denotes the same region.
 * - A pure pan: the live viewport. The pixels are a rigid screen translation of
 *   themselves, so the document tracks the pointer at UI frame rate.
 * - A zoom: the live viewport while \ref LivePlacementCoversEpochCoverage holds,
 *   so zoom motion tracks the fingers with sharpness catching up one epoch
 *   later; the epoch's transform otherwise.
 *
 * @param frame Per-frame presentation inputs.
 */
[[nodiscard]] const ViewportState& DirectSurfacePlacementViewport(
    const DocumentPresentationFrame& frame);

/**
 * Return whether a presentation-only frame may publish a newly accepted epoch.
 *
 * A presentation-only frame skips the ImGui pass, so selection chrome and the
 * compositor tile overlay keep the screen-space placement the last full frame
 * rasterized them at. Publishing an epoch that lands the document somewhere else
 * slides the document out from under its own chrome. The caller must wake a full
 * frame instead of publishing.
 *
 * @param lastFullFramePresentedViewport Viewport the last full frame presented
 *   document pixels with, or `std::nullopt` when no full frame has presented.
 * @param candidatePresentedViewport Viewport this frame would present with.
 */
[[nodiscard]] bool PresentationOnlyFrameMayPlaceEpoch(
    const std::optional<ViewportState>& lastFullFramePresentedViewport,
    const ViewportState& candidatePresentedViewport);

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
 * underneath the ImGui UI. The only presenter, on every platform.
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
 * Build the presenter every platform presents through.
 *
 * @param planSink Installs or clears the framebuffer underlay draw plan.
 */
[[nodiscard]] std::unique_ptr<DocumentPresenter> MakeDocumentPresenter(
    FramebufferUnderlayPlanSink planSink);

}  // namespace donner::editor
