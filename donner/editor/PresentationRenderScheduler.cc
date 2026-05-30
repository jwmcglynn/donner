#include "donner/editor/PresentationRenderScheduler.h"

#include <cstddef>

#include "donner/base/MathUtils.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"

namespace donner::editor {
namespace {

bool SameTransform(const Transform2d& lhs, const Transform2d& rhs) {
  constexpr double kTolerance = 1e-6;
  for (std::size_t i = 0; i < 6; ++i) {
    if (!NearEquals(lhs.data[i], rhs.data[i], kTolerance)) {
      return false;
    }
  }
  return true;
}

bool SameRasterViewport(const EditorRasterViewport& lhs, const EditorRasterViewport& rhs) {
  return lhs.documentRect == rhs.documentRect && lhs.outputSizePx == rhs.outputSizePx &&
         lhs.semanticCanvasSizePx == rhs.semanticCanvasSizePx &&
         lhs.viewportBounded == rhs.viewportBounded &&
         SameTransform(lhs.outputFromDocument, rhs.outputFromDocument);
}

}  // namespace

void PresentationRenderScheduler::reset() {
  lastRenderedVersion_ = 0;
  lastRenderedCanvasSize_ = Vector2i::Zero();
  lastRenderedRasterViewport_.reset();
}

PresentationRenderScheduleDecision PresentationRenderScheduler::evaluate(
    CompositedPresentation& presentation, const PresentationRenderScheduleInput& input) const {
  presentation.clearSettlingIfSelectionChanged(input.selectedEntity,
                                               input.activeDragPreview.has_value());

  PresentationRenderScheduleDecision decision;
  decision.currentVersion = input.currentVersion;
  decision.currentCanvasSize = input.currentCanvasSize;
  decision.currentRasterViewport = input.currentRasterViewport;
  decision.needsCompositedLayerCapture = presentation.needsCompositedLayerCapture(
      input.activeDragPreview, input.currentVersion, input.currentCanvasSize);
  const bool versionChanged = input.currentVersion != lastRenderedVersion_;
  const bool canvasSizeChanged = input.currentCanvasSize != lastRenderedCanvasSize_;
  const bool rasterViewportChanged =
      !lastRenderedRasterViewport_.has_value() ||
      !SameRasterViewport(input.currentRasterViewport, *lastRenderedRasterViewport_);
  const bool needsSettledSelectionRefresh =
      !input.activeDragPreview.has_value() &&
      presentation.needsSettledSelectionRefresh(input.selectedEntity, input.currentVersion);
  // A selected element needs a selection-hint render whenever the raster window changes. Otherwise
  // a high-zoom pan/zoom regular render can publish a full-canvas fallback and evict the promoted
  // drag-target tile just before the next drag starts.
  const bool selectedViewportRenderNeedsPrewarm = input.selectedEntity != entt::null &&
                                                  !input.activeDragPreview.has_value() &&
                                                  (canvasSizeChanged || rasterViewportChanged);
  decision.needsCompositedPrewarm =
      needsSettledSelectionRefresh ||
      presentation.shouldPrewarm(input.selectedEntity, input.selectedExtraEntities,
                                 input.currentVersion, input.currentCanvasSize,
                                 /*dragActive=*/input.activeDragPreview.has_value()) ||
      selectedViewportRenderNeedsPrewarm;
  decision.needsRegularRender = versionChanged || canvasSizeChanged || rasterViewportChanged;

  if (input.activeDragPreview.has_value()) {
    decision.dragPreview = RenderRequest::DragPreview{
        .entity = input.activeDragPreview->entity,
        .extraEntities = input.activeDragPreview->extraEntities,
        .interactionKind = svg::compositor::InteractionHint::ActiveDrag,
        .translation = input.activeDragPreview->translation,
        .documentFromCachedDocument = input.activeDragPreview->documentFromCachedDocument,
        .dragGeneration = input.activeDragPreview->dragGeneration,
    };
  } else if (decision.needsCompositedPrewarm && input.selectedEntity != entt::null) {
    decision.dragPreview = RenderRequest::DragPreview{
        .entity = input.selectedEntity,
        .extraEntities = input.selectedExtraEntities,
        .interactionKind = svg::compositor::InteractionHint::Selection,
    };
  }
  if (input.activeDragPreview.has_value()) {
    decision.needsRegularRender = false;
  }

  return decision;
}

void PresentationRenderScheduler::noteRenderCompleted(
    std::uint64_t completedVersion, const Vector2i& completedCanvasSize,
    const EditorRasterViewport& completedRasterViewport) {
  lastRenderedVersion_ = completedVersion;
  lastRenderedCanvasSize_ = completedCanvasSize;
  lastRenderedRasterViewport_ = completedRasterViewport;
}

}  // namespace donner::editor
