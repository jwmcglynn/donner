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
    editor.setSelection(entt::null);
    return;
  }

  const Entity entity = hit->entityHandle().entity();
  editor.setSelection(entity);

  // Capture the element's current parent-from-element transform so we can
  // compose deltas relative to it during the drag.
  const Transform2d startTransform =
      hit->cast<svg::SVGGraphicsElement>().transform();

  dragState_ = DragState{
      .entity = entity,
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
      EditorCommand::SetTransformCommand(dragState_->entity, newTransform));
}

void SelectTool::onMouseUp(EditorApp& editor, const Vector2d& /*documentPoint*/) {
  if (!dragState_.has_value()) {
    return;
  }

  // Record a single undo entry for the whole drag, but only if the
  // element actually moved — a click that never saw a mouse-move event
  // is a no-op for undo purposes.
  if (dragState_->hasMoved) {
    // Re-hit the drag-start point to recover an SVGElement handle we can
    // attach to the UndoSnapshot. Going back through `hitTest` avoids
    // SVGElement's protected constructor.
    if (auto hit = editor.hitTest(dragState_->startDocumentPoint); hit.has_value()) {
      UndoSnapshot before{.element = *hit, .transform = dragState_->startTransform};
      UndoSnapshot after{.element = *hit, .transform = dragState_->currentTransform};
      editor.undoTimeline().record("Move element", std::move(before), std::move(after));
    }
  }

  dragState_.reset();
}

}  // namespace donner::editor
