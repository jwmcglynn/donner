#include "donner/editor/DocumentPresenter.h"

#include <tuple>
#include <utility>

#include "donner/editor/RenderPanePresenter.h"

#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
#include <emscripten.h>
#endif

namespace donner::editor {

#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
// Positions the worker-owned document canvas from the live viewport. The
// bitmap-bridge and injected-hook forms take the whole layout object; the
// direct-canvas form writes CSS geometry onto the front/back canvas pair and
// stamps the accepted frame token so browser tests can observe which epoch is
// on screen.
EM_JS(void, UpdateWorkerDocumentSurfaceLayout,
      (int visible, int surfaceSlot, double left, double top, double width, double height,
       double clipLeft, double clipTop, double clipRight, double clipBottom, double frameCount,
       int selectionChromeBaked),
      {
        const layout = {
          visible : Boolean(visible),
          surfaceSlot,
          left,
          top,
          width,
          height,
          clipLeft,
          clipTop,
          clipRight,
          clipBottom,
          frameToken : Number(frameCount),
          selectionChromeBaked : Boolean(selectionChromeBaked),
        };
        if (window['__donnerWorkerSurfaceMode'] == 'bitmap-bridge' &&
            typeof Module['updateDonnerBitmapSurfaceLayout'] == 'function') {
          Module['updateDonnerBitmapSurfaceLayout'](layout);
          return;
        }
        if (typeof window['__donnerApplyWorkerDocumentSurfaceLayout'] == 'function') {
          window['__donnerApplyWorkerDocumentSurfaceLayout'](layout);
          return;
        }
        const canvases = [
          document.getElementById('donner-document-canvas'),
          document.getElementById('donner-document-canvas-back'),
        ];
        const visibleSlot = Math.max(0, Math.min(canvases.length - 1, surfaceSlot));
        for (let slot = 0; slot < canvases.length; ++slot) {
          const canvas = canvases[slot];
          if (!canvas) continue;
          const show = Boolean(visible) && width > 0 && height > 0 && slot == visibleSlot;
          canvas.setAttribute('data-direct-surface-visible', show ? 'true' : 'false');
          canvas.setAttribute('data-direct-surface-selection-chrome-baked',
                              show && selectionChromeBaked ? 'true' : 'false');
          if (!show) {
            canvas.style.visibility = 'hidden';
            continue;
          }
          canvas.style.left = String(left) + 'px';
          canvas.style.top = String(top) + 'px';
          canvas.style.width = String(width) + 'px';
          canvas.style.height = String(height) + 'px';
          canvas.style.clipPath = 'inset(' + clipTop + 'px ' + clipRight + 'px ' + clipBottom +
                                  'px ' + clipLeft + 'px)';
          canvas.style.visibility = 'visible';
          canvas.setAttribute('data-direct-surface-frame', String(frameCount));
        }
      });
#endif

WorkerSurfaceLayoutSink DefaultWorkerSurfaceLayoutSink() {
#if defined(__EMSCRIPTEN__) && defined(DONNER_EDITOR_WGPU)
  return [](const WorkerSurfaceLayout& layout) {
    UpdateWorkerDocumentSurfaceLayout(
        layout.visible ? 1 : 0, layout.surfaceSlot, layout.left, layout.top, layout.width,
        layout.height, layout.clipLeft, layout.clipTop, layout.clipRight, layout.clipBottom,
        static_cast<double>(layout.frameToken), layout.selectionChromeBaked ? 1 : 0);
  };
#else
  return [](const WorkerSurfaceLayout&) {};
#endif
}

namespace {

const ViewportState& WorkerSurfacePlacementViewport(const DocumentPresentationFrame& frame) {
  // Place the accepted pixels with the transform they were rasterized against.
  //
  // The live viewport is one or more worker frames ahead during a zoom or pan
  // gesture. A viewport-bounded high-zoom raster only covers the pane plus
  // `kHighZoomRasterMarginScreenPx`, so mapping its document rect through a
  // later, zoomed-out viewport shrinks the surface below the pane and uncovers
  // the editor background inside the render pane. Using the epoch's own
  // viewport keeps geometry and pixels changing together: the surface holds its
  // last valid placement until the next epoch replaces both at once.
  //
  // Epochs published before this field existed (and worker fallbacks that never
  // saw a live viewport) carry a degenerate viewport; those fall back to the
  // live transform, which is the pre-existing behavior.
  if (!DirectSurfacePlacementViewportIsUsable(frame.workerSurface.viewport)) {
    return frame.viewport;
  }
  // A pure pan is a rigid screen translation of the same pixels: follow the
  // live viewport so the document tracks the pointer at UI frame rate, exactly
  // like the desktop underlay composites the last raster at the live
  // transform. The epoch pin below remains for zoom / DPR / view-box changes,
  // where re-mapping a (possibly viewport-bounded) raster through a different
  // scale shrinks or shifts it off the pane and uncovers the background.
  const ViewportState& epoch = frame.workerSurface.viewport;
  const ViewportState& live = frame.viewport;
  if (live.zoom == epoch.zoom && live.devicePixelRatio == epoch.devicePixelRatio &&
      live.documentViewBox.topLeft == epoch.documentViewBox.topLeft &&
      live.documentViewBox.bottomRight == epoch.documentViewBox.bottomRight) {
    return live;
  }
  return epoch;
}

}  // namespace

bool DocumentPresentationMappingChanged(const ViewportState& before, const ViewportState& after) {
  return before.zoom != after.zoom || before.panDocPoint != after.panDocPoint ||
         before.panScreenPoint != after.panScreenPoint ||
         before.documentViewBox.topLeft != after.documentViewBox.topLeft ||
         before.documentViewBox.bottomRight != after.documentViewBox.bottomRight ||
         before.devicePixelRatio != after.devicePixelRatio;
}

std::optional<WorkerSurfaceLayout> ComputeWorkerSurfaceLayout(
    const DocumentPresentationFrame& frame) {
  if (frame.presentationSuppressed || !frame.workerSurface.active) {
    return std::nullopt;
  }

  const Box2d surfaceRect = WorkerSurfacePlacementViewport(frame).documentToScreen(
      frame.workerSurface.rasterViewport.documentRect);
  const std::optional<Box2d> clippedSurfaceRect =
      PresentedImageClipRect(frame.paneRect, surfaceRect);
  if (!clippedSurfaceRect.has_value() || surfaceRect.width() <= 0.0 ||
      surfaceRect.height() <= 0.0) {
    return std::nullopt;
  }

  // The surface backing store may be configured larger than the epoch's
  // content raster (it is sized once at the viewport's raster cap so zoom
  // gestures never resize-and-clear the canvas). The element box must span
  // the whole backing store so the content region - the top-left
  // contentSizePx of it - lands exactly on surfaceRect; the surplus band on
  // the right and bottom is clipped away.
  double surplusScreenX = 0.0;
  double surplusScreenY = 0.0;
  const Vector2i contentSizePx = frame.workerSurface.rasterViewport.outputSizePx;
  const Vector2i backingSizePx = frame.workerSurface.surfaceBackingSizePx;
  if (contentSizePx.x > 0 && contentSizePx.y > 0 && backingSizePx.x > contentSizePx.x) {
    surplusScreenX = surfaceRect.width() *
                     static_cast<double>(backingSizePx.x - contentSizePx.x) /
                     static_cast<double>(contentSizePx.x);
  }
  if (contentSizePx.x > 0 && contentSizePx.y > 0 && backingSizePx.y > contentSizePx.y) {
    surplusScreenY = surfaceRect.height() *
                     static_cast<double>(backingSizePx.y - contentSizePx.y) /
                     static_cast<double>(contentSizePx.y);
  }

  return WorkerSurfaceLayout{
      .visible = true,
      .surfaceSlot = frame.workerSurface.surfaceSlot,
      .left = surfaceRect.topLeft.x,
      .top = surfaceRect.topLeft.y,
      .width = surfaceRect.width() + surplusScreenX,
      .height = surfaceRect.height() + surplusScreenY,
      .clipLeft = clippedSurfaceRect->topLeft.x - surfaceRect.topLeft.x,
      .clipTop = clippedSurfaceRect->topLeft.y - surfaceRect.topLeft.y,
      .clipRight = surfaceRect.bottomRight.x - clippedSurfaceRect->bottomRight.x + surplusScreenX,
      .clipBottom =
          surfaceRect.bottomRight.y - clippedSurfaceRect->bottomRight.y + surplusScreenY,
      .frameToken = frame.workerSurface.frameCount,
      .selectionChromeBaked = frame.workerSurface.selectionChromeBaked,
  };
}

FramebufferUnderlayPresenter::FramebufferUnderlayPresenter(FramebufferUnderlayPlanSink planSink)
    : planSink_(std::move(planSink)) {}

DocumentPresentationResult FramebufferUnderlayPresenter::resolveExternalSurface(
    const DocumentPresentationFrame& frame) {
  frameOpen_ = true;
  // The underlay draws this frame's tiles with the live transform, so the
  // presented pixels are in the live viewport by construction.
  return DocumentPresentationResult{.presentedViewport = frame.viewport};
}

bool FramebufferUnderlayPresenter::presentUnderlay(std::optional<FramebufferUnderlayPlan> plan) {
  if (!frameOpen_) {
    ++refusedUnderlayPresents_;
    return false;
  }
  frameOpen_ = false;

  // Clear first on every frame: a plan installed last frame must never survive
  // into a frame that decided against the underlay.
  const bool present = plan.has_value();
  if (planSink_) {
    planSink_(std::nullopt);
    if (present) {
      planSink_(std::move(plan));
    }
  }
  return present;
}

WorkerSurfacePresenter::WorkerSurfacePresenter(WorkerSurfaceLayoutSink layoutSink,
                                               std::unique_ptr<DocumentPresenter> fallback)
    : layoutSink_(std::move(layoutSink)), fallback_(std::move(fallback)) {}

DocumentPresentationResult WorkerSurfacePresenter::resolveExternalSurface(
    const DocumentPresentationFrame& frame) {
  frameOpen_ = true;

  std::optional<WorkerSurfaceLayout> layout = ComputeWorkerSurfaceLayout(frame);
  bool heldViewportIsPresented = false;
  constexpr int kMaxHeldFrames = 120;
  if (!layout.has_value() && !frame.presentationSuppressed && !frame.workerSurface.active &&
      heldLayout_.has_value() && heldFrameCount_ < kMaxHeldFrames) {
    // A full document replacement invalidates the accepted epoch the instant the
    // new document loads, before any replacement epoch exists. Hiding the
    // surface there drops the render pane to the bare editor background for
    // every frame until the new document's first worker frame lands. The
    // worker canvas still holds the outgoing document's pixels - it is only
    // cleared inside the same worker task that presents the replacement - so
    // hold the last accepted placement instead. The loading affordance and the
    // new document's first epoch both land on top of it.
    layout = *heldLayout_;
    heldViewportIsPresented = true;
    ++heldFrameCount_;
  }
  // The hidden update carries the same frame token as a visible one so the
  // surface's accepted epoch stays observable across frames it does not own.
  const WorkerSurfaceLayout applied =
      layout.value_or(WorkerSurfaceLayout{.visible = false,
                                          .surfaceSlot = frame.workerSurface.surfaceSlot,
                                          .frameToken = frame.workerSurface.frameCount});
  if (frame.workerSurface.active && layout.has_value()) {
    heldLayout_ = applied;
    // The held placement is only meaningful together with the transform that
    // produced it: anything drawn onto those pixels while they are held has to
    // use the same viewport.
    heldViewport_ = WorkerSurfacePlacementViewport(frame);
    heldFrameCount_ = 0;
  } else if (frame.presentationSuppressed) {
    // The sample picker and content-only captures own the pane outright; nothing
    // accepted survives them.
    heldLayout_.reset();
    heldViewport_.reset();
  }
  lastLayout_ = applied;
  if (layoutSink_) {
    layoutSink_(applied);
  }

  surfaceOwnsFrame_ = layout.has_value();
  if (fallback_ != nullptr) {
    std::ignore = fallback_->resolveExternalSurface(frame);
  }

  // On a frame the worker surface owns, document pixels sit in the accepted
  // epoch's transform (or the held one while a document swap replays it).
  // Otherwise the underlay drew them live.
  ViewportState presentedViewport = frame.viewport;
  if (surfaceOwnsFrame_) {
    presentedViewport = heldViewportIsPresented && heldViewport_.has_value()
                            ? *heldViewport_
                            : WorkerSurfacePlacementViewport(frame);
  }

  return DocumentPresentationResult{
      .documentPresentedDirectly = surfaceOwnsFrame_,
      .externalSurfacePresented = surfaceOwnsFrame_,
      .selectionChromeBaked = surfaceOwnsFrame_ && applied.selectionChromeBaked,
      .presentedViewport = std::move(presentedViewport),
  };
}

bool WorkerSurfacePresenter::presentUnderlay(std::optional<FramebufferUnderlayPlan> plan) {
  if (!frameOpen_) {
    ++refusedUnderlayPresents_;
    return false;
  }
  frameOpen_ = false;

  if (fallback_ == nullptr) {
    return false;
  }
  // A frame the worker surface owns must leave the framebuffer underlay
  // cleared: two surfaces presenting the same document would double-draw.
  return fallback_->presentUnderlay(surfaceOwnsFrame_ ? std::nullopt : std::move(plan));
}

std::unique_ptr<DocumentPresenter> MakeDocumentPresenter(DocumentPresentationTarget target,
                                                         FramebufferUnderlayPlanSink planSink,
                                                         WorkerSurfaceLayoutSink layoutSink) {
  if (target == DocumentPresentationTarget::FramebufferUnderlay) {
    return std::make_unique<FramebufferUnderlayPresenter>(std::move(planSink));
  }
  return std::make_unique<WorkerSurfacePresenter>(
      std::move(layoutSink), std::make_unique<FramebufferUnderlayPresenter>(std::move(planSink)));
}

}  // namespace donner::editor
