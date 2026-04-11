#include "donner/editor/SelectTool.h"

#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
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

  // Coalescing happens in CommandQueue::flush(): the high-frequency drag
  // produces one SetTransform per mouse-move event but the queue collapses
  // them into a single effective command per entity per frame.
  editor.applyMutation(
      EditorCommand::SetTransformCommand(dragState_->entity, newTransform));
}

void SelectTool::onMouseUp(EditorApp& editor, const Vector2d& /*documentPoint*/) {
  // M2: drag commit is implicit — the last queued SetTransform is the final
  // state. Future milestones add UndoTimeline integration here, recording
  // a single undo entry for the whole drag (begin/commit transaction).
  (void)editor;
  dragState_.reset();
}

}  // namespace donner::editor
