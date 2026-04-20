#include "donner/editor/SelectTool.h"

#include <algorithm>
#include <vector>

#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {

namespace {

/// Walk @p leaf's ancestor chain and return the outermost ancestor whose
/// presentation attributes break the compositor's promotion invariants —
/// `filter`, `mask`, or `clip-path` as an inline attribute. If the entire
/// chain is clear, returns @p leaf unchanged.
///
/// Motivation: clicking a path that lives inside `<g filter="url(#blur)">`
/// hit-tests to the path, but `CompositorController::promoteEntity` refuses
/// to promote any descendant of a compositing-breaking ancestor (correctly
/// — promoting the leaf would bake the path into its own bitmap and lose
/// the ancestor filter's contribution). The compositor then falls through
/// to the full-document render path at every drag frame, which on the
/// splash costs ~250 ms per frame — "really laggy". Elevating the drag
/// target to the filter group itself matches user intent ("move the blurred
/// group") AND lets the compositor promote it into its own cached bitmap,
/// so steady-state drag frames drop to ~15 ms via the translation-only
/// fast path. Figma, Illustrator, and Inkscape all select the filter/mask
/// group when the user clicks a descendant, for the same reasons.
///
/// We elevate to the OUTERMOST such ancestor so nested filter-group chains
/// (`<g filter><g filter>…</g></g>`) still promote cleanly: the innermost
/// group would itself be refused by `HasCompositingBreakingAncestor`.
///
/// We check inline attributes only, not resolved CSS-derived filters —
/// intentional: `onMouseDown` fires on any click, including before the
/// next `renderFrame` has had a chance to resolve styles. Inline
/// attributes are available immediately after parse and catch the common
/// case. Missing a CSS-applied filter here means that rare case still
/// falls into the slow path; not a correctness issue, just a perf one.
svg::SVGElement ElevateToCompositingGroupAncestor(svg::SVGElement leaf) {
  svg::SVGElement best = leaf;
  svg::SVGElement cursor = leaf;
  while (auto parent = cursor.parentElement()) {
    if (parent->hasAttribute(xml::XMLQualifiedNameRef("filter")) ||
        parent->hasAttribute(xml::XMLQualifiedNameRef("mask")) ||
        parent->hasAttribute(xml::XMLQualifiedNameRef("clip-path"))) {
      best = *parent;
    }
    cursor = *parent;
  }
  return best;
}

}  // namespace

void SelectTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                             MouseModifiers modifiers) {
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
  // multi-element set, not moving anything. Leaf-accuracy matters here
  // so the user can add/remove individual paths from a set — no
  // compositing-group elevation.
  if (modifiers.shift) {
    editor.toggleInSelection(*hit);
    return;
  }

  // Plain click on an element. If the hit-tested leaf is a descendant
  // of a `<g filter>` / `<g mask>` / `<g clip-path>` group, elevate to
  // that group — both because the user's intent is to move the visible
  // filtered shape (not a single internal path that can't exist
  // visually on its own post-filter) AND because the compositor can
  // only promote the outermost compositing group into its own cached
  // layer. Without elevation the drag falls back to full-document
  // rendering at ~250 ms / frame on the splash.
  svg::SVGElement element = ElevateToCompositingGroupAncestor(*hit);

  // If the element is already in the current multi-selection, preserve
  // the selection and drag ALL selected elements in lockstep — classic
  // design-tool behavior (grab any item in the group, the group moves).
  // Otherwise replace the selection with just this element and drag it
  // alone.
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
          .sourceTransformAttributeValue = selected.getAttribute("transform"),
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
              .sourceTransformAttributeValue = element.getAttribute("transform"),
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

  // DOM is the source of truth during drag. Every drag frame applies a
  // `SetTransformCommand` for the primary AND every extra, regardless of
  // whether the compositor preview path is active. The composited path
  // optimizes the *visual* cost (the compositor detects a pure-translation
  // delta on a promoted layer and reuses the cached bitmap via its
  // internal composition transform instead of re-rasterizing — see
  // `CompositorController` fast-path), but the DOM writes happen either
  // way so the canvas view and the backing document never disagree. That
  // disagreement was the source of the drag-release "pop back" class of
  // bugs where the cached bitmap offset diverged from the DOM transform.
  editor.applyMutation(
      EditorCommand::SetTransformCommand(dragState_->primary.element, primaryNewTransform));
  for (auto& extra : dragState_->extras) {
    extra.currentTransform = Transform2d::Translate(deltaDoc) * extra.startTransform;
    editor.applyMutation(EditorCommand::SetTransformCommand(extra.element, extra.currentTransform));
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
  // is a no-op for undo purposes. `onMouseMove` has already been
  // applying `SetTransformCommand` every frame, so the DOM is already
  // at the final position; we only need to record the undo snapshot
  // and latch the writeback target here.
  if (dragState_->hasMoved) {
    UndoSnapshot before{
        .element = dragState_->primary.element,
        .transform = dragState_->primary.startTransform,
        .writebackTarget = dragState_->primary.writebackTarget,
        .sourceTransformAttributeValue = dragState_->primary.sourceTransformAttributeValue,
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
          .sourceTransformAttributeValue = extra.sourceTransformAttributeValue,
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
