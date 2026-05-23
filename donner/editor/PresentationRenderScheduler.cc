#include "donner/editor/PresentationRenderScheduler.h"

#include "donner/svg/compositor/ScopedCompositorHint.h"

namespace donner::editor {

void PresentationRenderScheduler::reset() {
  lastRenderedVersion_ = 0;
  lastRenderedCanvasSize_ = Vector2i::Zero();
}

PresentationRenderScheduleDecision PresentationRenderScheduler::evaluate(
    CompositedPresentation& presentation, const PresentationRenderScheduleInput& input) const {
  presentation.clearSettlingIfSelectionChanged(input.selectedEntity,
                                               input.activeDragPreview.has_value());

  PresentationRenderScheduleDecision decision;
  decision.currentVersion = input.currentVersion;
  decision.currentCanvasSize = input.currentCanvasSize;
  decision.needsCompositedLayerCapture = presentation.needsCompositedLayerCapture(
      input.activeDragPreview, input.currentVersion, input.currentCanvasSize);
  const bool needsSettledSelectionRefresh =
      !input.activeDragPreview.has_value() &&
      presentation.needsSettledSelectionRefresh(input.selectedEntity, input.currentVersion);
  decision.needsCompositedPrewarm =
      needsSettledSelectionRefresh ||
      presentation.shouldPrewarm(input.selectedEntity, input.currentVersion,
                                 input.currentCanvasSize,
                                 /*dragActive=*/false);
  decision.needsRegularRender = input.currentVersion != lastRenderedVersion_ ||
                                input.currentCanvasSize != lastRenderedCanvasSize_;

  if (input.activeDragPreview.has_value()) {
    decision.dragPreview = RenderRequest::DragPreview{
        .entity = input.activeDragPreview->entity,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
        .translation = input.activeDragPreview->translation,
        .dragGeneration = input.activeDragPreview->dragGeneration,
    };
  } else if (decision.needsCompositedPrewarm && input.selectedEntity != entt::null) {
    decision.dragPreview = RenderRequest::DragPreview{
        .entity = input.selectedEntity,
        .interactionKind = svg::compositor::InteractionHint::Selection,
    };
  }

  return decision;
}

void PresentationRenderScheduler::noteRenderCompleted(std::uint64_t completedVersion,
                                                      const Vector2i& completedCanvasSize) {
  lastRenderedVersion_ = completedVersion;
  lastRenderedCanvasSize_ = completedCanvasSize;
}

}  // namespace donner::editor
