#include "donner/editor/DocumentPresenter.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

#include "donner/editor/RenderPanePresenter.h"

namespace donner::editor {

namespace {

/// Screen-pixel slack allowed when asking whether a re-mapped surface still
/// covers what it used to. The two rects come out of separate floating-point
/// transform chains, so an exact containment test rejects placements that are
/// identical to well under a pixel.
constexpr double kCoverageSlackScreenPx = 1.0 / 64.0;

bool ViewportsShareTheSameDocumentFrame(const ViewportState& a, const ViewportState& b) {
  return a.devicePixelRatio == b.devicePixelRatio &&
         a.documentViewBox.topLeft == b.documentViewBox.topLeft &&
         a.documentViewBox.bottomRight == b.documentViewBox.bottomRight;
}

}  // namespace

bool LivePlacementCoversEpochCoverage(const Box2d& paneRect, const Box2d& epochSurfaceRect,
                                      const Box2d& liveSurfaceRect) {
  if (!std::isfinite(liveSurfaceRect.topLeft.x) || !std::isfinite(liveSurfaceRect.topLeft.y) ||
      !std::isfinite(liveSurfaceRect.bottomRight.x) ||
      !std::isfinite(liveSurfaceRect.bottomRight.y)) {
    return false;
  }

  // Only pane area counts: whatever the epoch placed outside the render pane
  // was clipped away and was never on screen to lose.
  const Box2d epochPaneCoverage(
      Vector2d(std::max(paneRect.topLeft.x, epochSurfaceRect.topLeft.x),
               std::max(paneRect.topLeft.y, epochSurfaceRect.topLeft.y)),
      Vector2d(std::min(paneRect.bottomRight.x, epochSurfaceRect.bottomRight.x),
               std::min(paneRect.bottomRight.y, epochSurfaceRect.bottomRight.y)));
  if (epochPaneCoverage.width() <= 0.0 || epochPaneCoverage.height() <= 0.0) {
    return true;
  }

  return liveSurfaceRect.topLeft.x <= epochPaneCoverage.topLeft.x + kCoverageSlackScreenPx &&
         liveSurfaceRect.topLeft.y <= epochPaneCoverage.topLeft.y + kCoverageSlackScreenPx &&
         liveSurfaceRect.bottomRight.x >=
             epochPaneCoverage.bottomRight.x - kCoverageSlackScreenPx &&
         liveSurfaceRect.bottomRight.y >= epochPaneCoverage.bottomRight.y - kCoverageSlackScreenPx;
}

const ViewportState& DirectSurfacePlacementViewport(const DocumentPresentationFrame& frame) {
  // Epochs published before this field existed (and worker fallbacks that never
  // saw a live viewport) carry a degenerate viewport; those fall back to the
  // live transform, which is the pre-existing behavior.
  if (!DirectSurfacePlacementViewportIsUsable(frame.workerSurface.viewport)) {
    return frame.viewport;
  }

  const ViewportState& epoch = frame.workerSurface.viewport;
  const ViewportState& live = frame.viewport;
  if (!ViewportsShareTheSameDocumentFrame(live, epoch)) {
    // A view-box or DPR change re-frames the document itself, so the accepted
    // raster's document rect no longer denotes the same screen region. Hold the
    // epoch's own placement until its replacement lands.
    return epoch;
  }

  // A pure pan is a rigid screen translation of the same pixels: follow the
  // live viewport so the document tracks the pointer at UI frame rate, exactly
  // like the desktop underlay composites its last raster at the live transform.
  if (live.zoom == epoch.zoom) {
    return live;
  }

  // Zoom. The accepted pixels are a fixed document rect, so mapping that rect
  // through the live transform scales the element and makes zoom motion track
  // the fingers at UI frame rate, with sharpness catching up one epoch later -
  // again the desktop underlay's contract.
  //
  // The clamp is the hazard the epoch pin exists for: a viewport-bounded
  // high-zoom raster only covers the pane plus
  // `kHighZoomRasterMarginScreenPx`, so re-mapping it through a zoomed-out live
  // viewport shrinks it inside the pane and uncovers the editor background
  // where document pixels used to be. Take the live viewport only while it
  // still covers every pane pixel the epoch's own placement covered; otherwise
  // hold the epoch placement, where geometry and pixels change together.
  const Box2d documentRect = frame.workerSurface.rasterViewport.documentRect;
  if (LivePlacementCoversEpochCoverage(frame.paneRect, epoch.documentToScreen(documentRect),
                                       live.documentToScreen(documentRect))) {
    return live;
  }
  return epoch;
}

bool PresentationOnlyFrameMayPlaceEpoch(
    const std::optional<ViewportState>& lastFullFramePresentedViewport,
    const ViewportState& candidatePresentedViewport) {
  // A presentation-only frame skips the ImGui pass entirely, so everything
  // drawn *onto* the document - selection chrome, the compositor tile overlay -
  // keeps the screen-space placement the last full frame rasterized it at.
  // Republishing an epoch that lands the document somewhere else slides the
  // document out from under its own chrome until some later frame rebuilds the
  // UI. Only epochs that place identically may be published from this path.
  return !lastFullFramePresentedViewport.has_value() ||
         !DocumentPresentationMappingChanged(*lastFullFramePresentedViewport,
                                             candidatePresentedViewport);
}

bool DocumentPresentationMappingChanged(const ViewportState& before, const ViewportState& after) {
  return before.zoom != after.zoom || before.panDocPoint != after.panDocPoint ||
         before.panScreenPoint != after.panScreenPoint ||
         before.documentViewBox.topLeft != after.documentViewBox.topLeft ||
         before.documentViewBox.bottomRight != after.documentViewBox.bottomRight ||
         before.devicePixelRatio != after.devicePixelRatio;
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

std::unique_ptr<DocumentPresenter> MakeDocumentPresenter(FramebufferUnderlayPlanSink planSink) {
  return std::make_unique<FramebufferUnderlayPresenter>(std::move(planSink));
}

}  // namespace donner::editor
