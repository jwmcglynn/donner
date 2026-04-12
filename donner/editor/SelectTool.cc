#include "donner/editor/SelectTool.h"

#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {

void SelectTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint) {
  // Reset any in-progress drag — a previous mouse-down without a matching
  // mouse-up means the user dragged off the window or a tool switch
  // happened mid-drag. Either way, abandon the old drag silently.
  dragState_.reset();

  auto hit = editor.hitTest(documentPoint);
  if (!hit.has_value()) {
    editor.setSelection(std::nullopt);
    return;
  }

  // hitTest returns an SVGGeometryElement, which is already an SVGElement
  // subclass. Store it directly in the drag state — the editor never
  // needs to unwrap to a raw Entity.
  svg::SVGElement element = *hit;

  // Capture the element's current parent-from-element transform so we can
  // compose deltas relative to it during the drag.
  const Transform2d startTransform =
      element.cast<svg::SVGGraphicsElement>().transform();

  editor.setSelection(element);

  dragState_ = DragState{
      .element = element,
      .startDocumentPoint = documentPoint,
      .startTransform = startTransform,
      .currentTransform = startTransform,
  };
}

void SelectTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) {
  if (!buttonHeld || !dragState_.has_value()) {
    return;
  }

  const Vector2d deltaDoc = documentPoint - dragState_->startDocumentPoint;

  // donner's Transform2 uses "row-major post-multiply" semantics: in
  // `R * T * A`, A is applied first and R last. To translate the element
  // in *parent* space (document space, since we assume top-level
  // elements), the new local transform is the old local transform
  // followed by a parent-space translation:
  //
  //   new_local = Translate(delta) * old_local
  const Transform2d newTransform =
      Transform2d::Translate(deltaDoc) * dragState_->startTransform;

  dragState_->currentTransform = newTransform;
  dragState_->hasMoved = true;

  // Coalescing happens in CommandQueue::flush(): the high-frequency drag
  // produces one SetTransform per mouse-move event but the queue collapses
  // them into a single effective command per entity per frame.
  editor.applyMutation(
      EditorCommand::SetTransformCommand(dragState_->element, newTransform));
}

void SelectTool::onMouseUp(EditorApp& editor, const Vector2d& /*documentPoint*/) {
  if (!dragState_.has_value()) {
    return;
  }

  // Record a single undo entry for the whole drag, but only if the
  // element actually moved — a click that never saw a mouse-move event
  // is a no-op for undo purposes.
  if (dragState_->hasMoved) {
    UndoSnapshot before{.element = dragState_->element, .transform = dragState_->startTransform};
    UndoSnapshot after{.element = dragState_->element, .transform = dragState_->currentTransform};
    editor.undoTimeline().record("Move element", std::move(before), std::move(after));

    // Signal the main loop that a drag completed and the `transform`
    // attribute should be written back into the source text. The flag
    // is consumed (cleared) by `consumeDragCompleted()`.
    dragCompleted_ = true;
  }

  dragState_.reset();
}

}  // namespace donner::editor
