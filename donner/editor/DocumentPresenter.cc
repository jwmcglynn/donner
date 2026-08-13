#include "donner/editor/DocumentPresenter.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>


namespace donner::editor {

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
