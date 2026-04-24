#include "donner/editor/backend_lib/SelectTool.h"

#include <algorithm>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/backend_lib/AttributeWriteback.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/backend_lib/EditorCommand.h"
#include "donner/editor/backend_lib/UndoTimeline.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/SVGSVGElement.h"

namespace donner::editor {

namespace {

/// Elements that don't contribute geometry to the rendered tree.
/// `<defs>` / `<title>` / `<desc>` / `<metadata>` / `<style>` / `<script>`
/// live under an SVG document but paint nothing — they must be excluded
/// when detecting "wrapping single-g containers" so the presence of a
/// `<defs>` block doesn't prevent a top-level `<g>` from being recognised
/// as the sole render child.
bool IsNonRenderChild(const svg::SVGElement& element) {
  const auto tag = element.tagName().name;
  return tag == RcString("defs") || tag == RcString("title") || tag == RcString("desc") ||
         tag == RcString("metadata") || tag == RcString("style") || tag == RcString("script");
}

/// Walk down from the document root through "wrapping" layers — each
/// level where the element has exactly one render-contributing child
/// that's a `<g>` — and return the deepest such container. The children
/// of this container are the document's **top-level objects**: the
/// logical units the editor treats as grouped for click/drag selection.
///
/// Rationale (design intent, post-#582): SVG artists routinely wrap
/// their whole document in one or more `<g>` layers (scale, paint order,
/// viewport coord reset, etc.) that carry no semantic weight. Treating
/// those wrappers as selection targets means clicks on any visible path
/// would select the entire document. Descending through them to reach
/// the *real* top-level grouping (e.g. `<g id="Donner">`,
/// `<g id="Lightning_glow_dark">`, `<g id="Big_lightning_glow">` on the
/// splash) matches what Figma / Illustrator / Inkscape do — and what a
/// user expects when they click on a composed object.
///
/// Stop conditions, each "top-level object(s) live here" signal:
///   * current element has ≠ 1 render child (zero or multiple distinct
///     objects at this level);
///   * the lone render child isn't a `<g>` (a terminal geometry element
///     like `<path>` is itself a top-level object);
///   * the lone child carries `filter` / `mask` / `clip-path` / `id`, or
///     any attribute the editor treats as semantic — those are the
///     "this wrapper IS a logical object" markers.
svg::SVGElement DeepestWrappingContainer(svg::SVGElement root) {
  svg::SVGElement current = root;
  while (true) {
    std::optional<svg::SVGElement> soleRenderChild;
    bool multipleRenderChildren = false;
    for (auto child = current.firstChild(); child.has_value(); child = child->nextSibling()) {
      if (IsNonRenderChild(*child)) continue;
      if (soleRenderChild.has_value()) {
        multipleRenderChildren = true;
        break;
      }
      soleRenderChild = *child;
    }
    if (multipleRenderChildren || !soleRenderChild.has_value()) return current;

    // Stop at non-`<g>` children (terminal geometry) and at `<g>`s that
    // carry semantic attributes — a `<g id="Foo">` or `<g filter="…">`
    // is itself a top-level object, not a wrapper.
    const auto tag = soleRenderChild->tagName().name;
    if (tag != RcString("g")) return current;
    if (!soleRenderChild->id().empty()) return current;
    if (soleRenderChild->hasAttribute(xml::XMLQualifiedNameRef("filter")) ||
        soleRenderChild->hasAttribute(xml::XMLQualifiedNameRef("mask")) ||
        soleRenderChild->hasAttribute(xml::XMLQualifiedNameRef("clip-path"))) {
      return current;
    }

    current = *soleRenderChild;
  }
}

/// True when @p element carries an inline `filter` / `mask` / `clip-path`
/// attribute — the markers the editor treats as "this group is a
/// compositing-aware object, not a plain transparent wrapper".
bool HasCompositingAttribute(const svg::SVGElement& element) {
  return element.hasAttribute(xml::XMLQualifiedNameRef("filter")) ||
         element.hasAttribute(xml::XMLQualifiedNameRef("mask")) ||
         element.hasAttribute(xml::XMLQualifiedNameRef("clip-path"));
}

/// Return the ancestor of @p hit that's a direct child of @p container, or
/// @p hit unchanged if no such ancestor exists (the hit either IS the
/// container or sits outside it entirely).
svg::SVGElement TopLevelAncestor(svg::SVGElement hit, const svg::SVGElement& container) {
  svg::SVGElement cursor = hit;
  while (auto parent = cursor.parentElement()) {
    if (*parent == container) {
      return cursor;
    }
    cursor = *parent;
  }
  return hit;
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

  // Plain click on an element. Two-step elevation:
  //
  //   1. Find the "top-level object" — the ancestor that sits directly
  //      under the deepest `<g>`-only wrapping container (see
  //      `DeepestWrappingContainer`). This skips past transparent
  //      `<g class="cls-94">`-style outer wrappers that exist only to
  //      group the paint order, so the editor treats their children
  //      as the document's top-level objects.
  //
  //   2. Elevate the click to that top-level object ONLY IF it carries
  //      a compositing attribute (`filter` / `mask` / `clip-path`).
  //      Compositing attributes mark a `<g>` as a unit whose
  //      descendants can't be individually dragged without visibly
  //      breaking the composition (the filter output depends on the
  //      whole subtree being rendered as one). Plain `<g>`s (no
  //      compositing attribute) leave the selection on the clicked
  //      leaf — a single Donner letter, a single cloud path, etc. —
  //      matching what a vector editor's direct-select tool does.
  //
  // TODO: double-click "focus into" to descend one level deeper in a
  // filter-group — select the clicked path instead of the whole group.
  // Requires double-click detection in the pointer-event protocol.
  svg::SVGDocument& doc = editor.document().document();
  const svg::SVGElement container = DeepestWrappingContainer(doc.svgElement());
  const svg::SVGElement topLevel = TopLevelAncestor(*hit, container);
  svg::SVGElement element = HasCompositingAttribute(topLevel) ? topLevel : *hit;

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
    editor.undoTimeline().record(dragState_->extras.empty() ? "Move element" : "Move elements",
                                 std::move(before), std::move(after));

    // Record undo for each extra element so a single Ctrl+Z reverts the
    // whole multi-element drag — one timeline entry per element keeps the
    // existing timeline plumbing intact; the user sees them collapsed by
    // the shared "Move elements" label if the timeline groups by label.
    for (const auto& extra : dragState_->extras) {
      UndoSnapshot extraBefore{.element = extra.element,
                               .transform = extra.startTransform,
                               .writebackTarget = extra.writebackTarget,
                               .sourceTransformAttributeValue = extra.sourceTransformAttributeValue,
                               .restoreSourceTransformAttributeValue = true};
      UndoSnapshot extraAfter{.element = extra.element,
                              .transform = extra.currentTransform,
                              .writebackTarget = extra.writebackTarget};
      editor.undoTimeline().record("Move elements", std::move(extraBefore), std::move(extraAfter));
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
  if (!compositedDragPreviewEnabled_ || !dragState_.has_value() || !dragState_->hasMoved) {
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
