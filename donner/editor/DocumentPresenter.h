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
};

/// What the presenter resolved for this frame.
struct DocumentPresentationResult {
  /// The viewport this frame's presented document pixels are placed with.
  ///
  /// Always the live viewport: the underlay draws this frame's tiles with it.
  /// The field survives the worker-surface deletion because every caller that
  /// draws *onto* the document - selection chrome, the compositor tile overlay,
  /// the presented image clip - must read the transform the pixels were placed
  /// with rather than assume one.
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
   * Stage 1: open the frame and resolve the transform this frame's document
   * pixels are placed with.
   *
   * @param frame Per-frame presentation inputs.
   * @return What this frame resolved to.
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
