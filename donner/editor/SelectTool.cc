#include "donner/editor/SelectTool.h"

#include <algorithm>
#include <vector>

#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {

void SelectTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                             MouseModifiers modifiers) {
  // Drain any pending commit from a previous drag release before we
  // start the next gesture. The new onMouseDown might start another
  // drag on the same entity — that drag expects the DOM's transform
  // attribute to reflect the previous drag's final position so its
  // startTransform reads correctly. Keeping the commit deferred past
  // this point is what was letting the drag release feel zero-cost.
  commitPendingDragMutation(editor);

  // Reset any in-progress drag/marquee — a previous mouse-down without
  // a matching mouse-up means the user dragged off the window or a
  // tool switch happened mid-drag. Either way, abandon the old gesture
  // silently.
  dragState_.reset();
  marqueeState_.reset();

  auto hit = editor.hitTest(documentPoint);

  // Click on empty space → start a marquee. The marquee resolves to a
  // selection set on `onMouseUp`. While dragging it shows up as
  // overlay chrome via `marqueeRect()`.
  if (!hit.has_value()) {
    if (!modifiers.shift) {
      // Plain click on empty space clears the selection up-front so
      // the user gets immediate visual feedback even if the drag
      // never grows past zero pixels (i.e. a quick miss-click).
      editor.clearSelection();
    }
    marqueeState_ = MarqueeState{
        .startDocumentPoint = documentPoint,
        .currentDocumentPoint = documentPoint,
        .additive = modifiers.shift,
    };
    return;
  }

  // Shift+click on an element → toggle membership in the current
  // selection without starting a drag. The user is curating a
  // multi-element set, not moving anything.
  svg::SVGElement element = *hit;
  if (modifiers.shift) {
    editor.toggleInSelection(element);
    return;
  }

  // Plain click on an element. If the element is already in the current
  // multi-selection, preserve the selection and drag ALL selected
  // elements in lockstep — classic design-tool behavior (grab any item
  // in the group, the group moves). Otherwise replace the selection
  // with just this element and drag it alone.
  const auto& currentSelection = editor.selectedElements();
  const bool elementAlreadySelected =
      std::any_of(currentSelection.begin(), currentSelection.end(),
                  [&element](const svg::SVGElement& selected) { return selected == element; });
  const bool isMultiDrag = elementAlreadySelected && currentSelection.size() > 1;

  const Transform2d primaryStartTransform = element.cast<svg::SVGGraphicsElement>().transform();
  const auto primaryWritebackTarget = captureAttributeWritebackTarget(element);

  std::vector<PerElementDrag> extras;
  if (isMultiDrag) {
    extras.reserve(currentSelection.size() - 1);
    for (const svg::SVGElement& selected : currentSelection) {
      if (selected == element) {
        continue;  // primary handled separately
      }
      const Transform2d extraStart = selected.cast<svg::SVGGraphicsElement>().transform();
      extras.push_back(PerElementDrag{
          .element = selected,
          .startTransform = extraStart,
          .currentTransform = extraStart,
          .writebackTarget = captureAttributeWritebackTarget(selected),
      });
    }
  } else {
    editor.setSelection(element);
  }

  dragState_ = DragState{
      .primary =
          PerElementDrag{
              .element = element,
              .startTransform = primaryStartTransform,
              .currentTransform = primaryStartTransform,
              .writebackTarget = primaryWritebackTarget,
          },
      .extras = std::move(extras),
      .startDocumentPoint = documentPoint,
  };
}

void SelectTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) {
  if (!buttonHeld) {
    return;
  }

  // Marquee drag: just update the current point. The overlay re-render
  // picks the new rect up via `marqueeRect()` next frame.
  if (marqueeState_.has_value()) {
    marqueeState_->currentDocumentPoint = documentPoint;
    return;
  }

  if (!dragState_.has_value()) {
    return;
  }

  const Vector2d deltaDoc = documentPoint - dragState_->startDocumentPoint;

  // Drag threshold: swallow sub-pixel jitter so a click that accidentally
  // moves 0.0001 document units between mouse-down and mouse-up doesn't
  // latch `hasMoved` and fire a near-identity writeback. The threshold is
  // checked against the squared delta magnitude in document units; once
  // the drag has actually moved past it the element tracks the cursor
  // exactly from that point on.
  constexpr double kDragThresholdDocUnits = 1.0;
  constexpr double kDragThresholdSq = kDragThresholdDocUnits * kDragThresholdDocUnits;
  if (!dragState_->hasMoved && deltaDoc.lengthSquared() < kDragThresholdSq) {
    return;
  }

  // donner's Transform2 uses "row-major post-multiply" semantics: in
  // `R * T * A`, A is applied first and R last. To translate the element
  // in *parent* space (document space, since we assume top-level
  // elements), the new local transform is the old local transform
  // followed by a parent-space translation:
  //
  //   new_local = Translate(delta) * old_local
  const Transform2d primaryNewTransform =
      Transform2d::Translate(deltaDoc) * dragState_->primary.startTransform;

  dragState_->currentDocumentDelta = deltaDoc;
  dragState_->primary.currentTransform = primaryNewTransform;
  dragState_->hasMoved = true;

  // Extras always move through the mutation path — compositing only tracks
  // the primary drag target. For the multi-element case we accept slightly
  // less smooth drag (DOM mutation + re-render each frame) in exchange for
  // shipping the feature without redesigning the drag-preview transport.
  for (auto& extra : dragState_->extras) {
    extra.currentTransform = Transform2d::Translate(deltaDoc) * extra.startTransform;
    editor.applyMutation(EditorCommand::SetTransformCommand(extra.element, extra.currentTransform));
  }

  // Composited drag preview only applies when there are no extras. With
  // extras, the primary must also flow through applyMutation so every
  // element updates in the same frame (otherwise the primary would be
  // frozen at its start position while extras move, which looks broken).
  const bool useCompositedPath = compositedDragPreviewEnabled_ && dragState_->extras.empty();
  if (!useCompositedPath) {
    editor.applyMutation(
        EditorCommand::SetTransformCommand(dragState_->primary.element, primaryNewTransform));
  }
}

void SelectTool::onMouseUp(EditorApp& editor, const Vector2d& /*documentPoint*/) {
  // Marquee resolution: convert the rect to a selection set. The tool
  // can only ever be in one of {drag, marquee} at a time, so both
  // branches are independent.
  if (marqueeState_.has_value()) {
    // Snapshot what we need from the marquee state *before* clearing
    // it, so the rest of this branch can read clean values without
    // tripping over a reset optional.
    const Box2d rect = *marqueeRect();
    const bool additive = marqueeState_->additive;
    marqueeState_.reset();

    auto hits = editor.hitTestRect(rect);
    std::vector<svg::SVGElement> hitsAsElements;
    hitsAsElements.reserve(hits.size());
    for (const auto& hit : hits) {
      hitsAsElements.emplace_back(hit);
    }

    if (additive) {
      // Shift+marquee appends to the existing selection. Use
      // `addToSelection` so duplicates collapse silently — a marquee
      // that re-grabs an already-selected element shouldn't toggle
      // it off.
      for (const auto& element : hitsAsElements) {
        editor.addToSelection(element);
      }
    } else {
      editor.setSelection(std::move(hitsAsElements));
    }
    return;
  }

  if (!dragState_.has_value()) {
    return;
  }

  // Record a single undo entry for the whole drag, but only if the
  // element actually moved — a click that never saw a mouse-move event
  // is a no-op for undo purposes.
  if (dragState_->hasMoved) {
    const bool useCompositedPath =
        compositedDragPreviewEnabled_ && dragState_->extras.empty();
    if (useCompositedPath) {
      // Zero-work drag release. Stash everything we'd otherwise do
      // synchronously here (applyMutation, undo record, writeback latch)
      // into `pendingCommit_`. `commitPendingDragMutation` drains it the
      // next time a user action requires a consistent DOM state (next
      // drag's onMouseDown, undo / redo, writeback consumer, etc).
      //
      // The compositor keeps showing the drag-final position via its
      // cached composited textures + the held drag-translate compose
      // offset (see `ExperimentalDragPresentation::settlingPreview`),
      // so the display is visually identical to a world where we'd
      // eagerly committed. The difference is exclusively on the CPU
      // side: no `applyMutation`, no `undoTimeline.record`, no tree
      // walk — so the mouse-release frame does essentially zero work.
      pendingCommit_ = PendingCommit{
          .element = dragState_->primary.element,
          .startTransform = dragState_->primary.startTransform,
          .currentTransform = dragState_->primary.currentTransform,
          .writebackTarget = dragState_->primary.writebackTarget,
          .sourceTransformAttributeValue =
              dragState_->primary.element.getAttribute("transform"),
          .undoLabel = "Move element",
      };
      dragState_.reset();
      return;
    }

    UndoSnapshot before{
        .element = dragState_->primary.element,
        .transform = dragState_->primary.startTransform,
        .writebackTarget = dragState_->primary.writebackTarget,
        .sourceTransformAttributeValue = dragState_->primary.element.getAttribute("transform"),
        .restoreSourceTransformAttributeValue = true};
    UndoSnapshot after{.element = dragState_->primary.element,
                       .transform = dragState_->primary.currentTransform,
                       .writebackTarget = dragState_->primary.writebackTarget};
    editor.undoTimeline().record(
        dragState_->extras.empty() ? "Move element" : "Move elements",
        std::move(before), std::move(after));

    // Record undo for each extra element so a single Ctrl+Z reverts the
    // whole multi-element drag — one timeline entry per element keeps the
    // existing timeline plumbing intact; the user sees them collapsed by
    // the shared "Move elements" label if the timeline groups by label.
    for (const auto& extra : dragState_->extras) {
      UndoSnapshot extraBefore{
          .element = extra.element,
          .transform = extra.startTransform,
          .writebackTarget = extra.writebackTarget,
          .sourceTransformAttributeValue = extra.element.getAttribute("transform"),
          .restoreSourceTransformAttributeValue = true};
      UndoSnapshot extraAfter{.element = extra.element,
                              .transform = extra.currentTransform,
                              .writebackTarget = extra.writebackTarget};
      editor.undoTimeline().record("Move elements", std::move(extraBefore),
                                   std::move(extraAfter));
    }

    if (dragState_->primary.writebackTarget.has_value()) {
      std::vector<CompletedDragWriteback> extraWritebacks;
      extraWritebacks.reserve(dragState_->extras.size());
      for (const auto& extra : dragState_->extras) {
        if (extra.writebackTarget.has_value()) {
          extraWritebacks.push_back(CompletedDragWriteback{
              .target = *extra.writebackTarget,
              .transform = extra.currentTransform,
          });
        }
      }
      completedDragWriteback_ = CompletedDragWriteback{
          .target = *dragState_->primary.writebackTarget,
          .transform = dragState_->primary.currentTransform,
          .extras = std::move(extraWritebacks),
      };
    }
  }

  dragState_.reset();
}

void SelectTool::commitPendingDragMutation(EditorApp& editor) {
  if (!pendingCommit_.has_value()) {
    return;
  }
  PendingCommit commit = std::move(*pendingCommit_);
  pendingCommit_.reset();

  editor.applyMutation(
      EditorCommand::SetTransformCommand(commit.element, commit.currentTransform));

  UndoSnapshot before{
      .element = commit.element,
      .transform = commit.startTransform,
      .writebackTarget = commit.writebackTarget,
      .sourceTransformAttributeValue = commit.sourceTransformAttributeValue,
      .restoreSourceTransformAttributeValue = true};
  UndoSnapshot after{.element = commit.element,
                     .transform = commit.currentTransform,
                     .writebackTarget = commit.writebackTarget};
  editor.undoTimeline().record(commit.undoLabel, std::move(before), std::move(after));

  if (commit.writebackTarget.has_value()) {
    completedDragWriteback_ = CompletedDragWriteback{
        .target = *commit.writebackTarget,
        .transform = commit.currentTransform,
    };
  }
}

std::optional<SelectTool::ActiveDragPreview> SelectTool::activeDragPreview() const {
  if (!compositedDragPreviewEnabled_ || !dragState_.has_value()) {
    return std::nullopt;
  }
  // Multi-element drags run through the mutation path (not compositor)
  // because the drag-preview transport only models a single moving layer.
  // When we have extras, there's no composited preview to report.
  if (!dragState_->extras.empty()) {
    return std::nullopt;
  }

  return ActiveDragPreview{.entity = dragState_->primary.element.entityHandle().entity(),
                           .translation = dragState_->currentDocumentDelta};
}

std::optional<Box2d> SelectTool::marqueeRect() const {
  if (!marqueeState_.has_value()) {
    return std::nullopt;
  }
  // Build a Box2d from the two end-points, normalizing so topLeft has
  // the smaller (x, y). `Box2d`'s constructor doesn't normalize, so a
  // marquee dragged up-and-left would otherwise produce an inverted
  // box that AABB intersection tests reject.
  const Vector2d a = marqueeState_->startDocumentPoint;
  const Vector2d b = marqueeState_->currentDocumentPoint;
  return Box2d(Vector2d(std::min(a.x, b.x), std::min(a.y, b.y)),
               Vector2d(std::max(a.x, b.x), std::max(a.y, b.y)));
}

}  // namespace donner::editor
