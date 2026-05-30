#include "donner/editor/SelectTool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/AttributeWriteback.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/UndoTimeline.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {

namespace {

constexpr double kDragThresholdDocUnits = 1.0;
constexpr double kDragThresholdSq = kDragThresholdDocUnits * kDragThresholdDocUnits;
constexpr double kMinScaleDenominator = 1e-9;
constexpr double kRedragHitSlopScreenPx = 2.0;

bool IsFinite(double value) {
  return std::isfinite(value);
}

bool IsFinite(const Transform2d& candidateDocumentFromDocument) {
  for (double value : candidateDocumentFromDocument.data) {
    if (!IsFinite(value)) {
      return false;
    }
  }
  return true;
}

Vector2d CenterOf(const Box2d& box) {
  return (box.topLeft + box.bottomRight) * 0.5;
}

Transform2d TransformDocumentAroundPoint(const Vector2d& fixedDocumentPoint,
                                         const Transform2d& centeredDocumentFromDocument) {
  return Transform2d::Translate(-fixedDocumentPoint) * centeredDocumentFromDocument *
         Transform2d::Translate(fixedDocumentPoint);
}

double AngleFromCenter(const Vector2d& center, const Vector2d& point) {
  const Vector2d delta = point - center;
  return std::atan2(delta.y, delta.x);
}

std::optional<Transform2d> ResizeTransform(const Box2d& startBounds,
                                           SelectionTransformCorner corner,
                                           const Vector2d& documentPoint, bool preserveAspectRatio,
                                           bool resizeFromCenter) {
  const Vector2d center = CenterOf(startBounds);
  const Vector2d startCorner = SelectionTransformCornerPoint(startBounds, corner);
  const Vector2d anchor =
      resizeFromCenter
          ? center
          : SelectionTransformCornerPoint(startBounds, OppositeSelectionTransformCorner(corner));
  const Vector2d startVector = startCorner - anchor;
  const Vector2d currentVector = documentPoint - anchor;
  if (std::abs(startVector.x) < kMinScaleDenominator ||
      std::abs(startVector.y) < kMinScaleDenominator) {
    return std::nullopt;
  }

  double scaleX = currentVector.x / startVector.x;
  double scaleY = currentVector.y / startVector.y;
  if (preserveAspectRatio) {
    const double uniformScale = std::abs(scaleX) >= std::abs(scaleY) ? scaleX : scaleY;
    scaleX = uniformScale;
    scaleY = uniformScale;
  }

  const Transform2d resizedDocumentFromStartDocument =
      TransformDocumentAroundPoint(anchor, Transform2d::Scale(scaleX, scaleY));
  if (!IsFinite(resizedDocumentFromStartDocument)) {
    return std::nullopt;
  }
  return resizedDocumentFromStartDocument;
}

std::optional<Transform2d> RotateTransform(const Vector2d& center, double startAngleRadians,
                                           const Vector2d& documentPoint) {
  const double angleDelta = AngleFromCenter(center, documentPoint) - startAngleRadians;
  const Transform2d rotatedDocumentFromStartDocument =
      TransformDocumentAroundPoint(center, Transform2d::Rotate(angleDelta));
  if (!IsFinite(rotatedDocumentFromStartDocument)) {
    return std::nullopt;
  }
  return rotatedDocumentFromStartDocument;
}

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
      if (IsNonRenderChild(*child)) {
        continue;
      }
      if (soleRenderChild.has_value()) {
        multipleRenderChildren = true;
        break;
      }
      soleRenderChild = *child;
    }
    if (multipleRenderChildren || !soleRenderChild.has_value()) {
      return current;
    }

    // Stop at non-`<g>` children (terminal geometry) and at `<g>`s that
    // carry semantic attributes — a `<g id="Foo">` or `<g filter="…">`
    // is itself a top-level object, not a wrapper.
    const auto tag = soleRenderChild->tagName().name;
    if (tag != RcString("g")) {
      return current;
    }
    if (!soleRenderChild->id().empty()) {
      return current;
    }
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

bool SelectTool::tryStartRedragOnSelected(EditorApp& editor, const Vector2d& documentPoint,
                                          MouseModifiers modifiers,
                                          std::span<const Box2d> selectionBoundsDoc,
                                          std::span<const Box2d> occludingBoundsDoc) {
  if (modifiers.shift) {
    return false;
  }
  const auto& currentSelection = editor.selectedElements();
  if (currentSelection.size() != 1) {
    return false;
  }
  // M8: bounds are caller-supplied so this path is race-safe during a
  // busy render. EditorShell passes the pre-snapshotted bounds from
  // `SelectionBoundsCache::displayedBoundsDoc` (no live registry read);
  // `onMouseDown` passes a freshly-computed live snapshot (its caller has
  // already gated on `!isBusy()`).
  const double hitSlopDoc =
      modifiers.pixelsPerDocUnit > 0.0 ? kRedragHitSlopScreenPx / modifiers.pixelsPerDocUnit : 0.0;
  if (selectionBoundsDoc.empty() ||
      !selectionBoundsDoc.front().inflatedBy(hitSlopDoc).contains(documentPoint) ||
      !currentSelection.front().isa<svg::SVGGraphicsElement>()) {
    return false;
  }
  for (const Box2d& occludingBounds : occludingBoundsDoc) {
    if (occludingBounds.contains(documentPoint)) {
      return false;
    }
  }

  // Reuse the currently-selected element as the drag target.
  const svg::SVGElement element = currentSelection.front();
  const bool isGraphics =
      element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
        return element.isa<svg::SVGGraphicsElement>();
      });
  if (!isGraphics) {
    return false;
  }
  const svg::SVGGraphicsElement graphicsElement =
      element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
        return element.cast<svg::SVGGraphicsElement>();
      });
  const Transform2d primaryStartTransform = graphicsElement.transform();
  const auto primaryWritebackTarget = captureAttributeWritebackTarget(element);
  const std::optional<RcString> sourceTransformAttributeValue =
      element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
        return element.getAttribute("transform");
      });

  // Reset any in-progress drag/marquee state before starting the new one
  // — mirrors `onMouseDown`'s prologue. A previous mouse-down without
  // a matching mouse-up means the user dragged off the window or a
  // tool switch happened mid-drag.
  dragState_.reset();
  marqueeState_.reset();

  dragState_ = DragState{
      .primary =
          PerElementDrag{
              .element = element,
              .startTransform = primaryStartTransform,
              .currentTransform = primaryStartTransform,
              .writebackTarget = primaryWritebackTarget,
              .sourceTransformAttributeValue = sourceTransformAttributeValue,
          },
      .extras = {},
      .startDocumentPoint = documentPoint,
      .startBoundsDoc = CombinedSelectionBounds(selectionBoundsDoc),
      .generation = nextDragGeneration_++,
  };
  return true;
}

bool SelectTool::clickHitsCurrentSelection(EditorApp& editor, const Vector2d& documentPoint) const {
  const auto& currentSelection = editor.selectedElements();
  if (currentSelection.empty()) {
    return false;
  }

  const std::optional<svg::SVGGeometryElement> hit = editor.hitTest(documentPoint);
  if (!hit.has_value()) {
    return false;
  }

  const svg::SVGElement hitElement = *hit;
  for (const svg::SVGElement& selected : currentSelection) {
    if (selected == hitElement) {
      return true;
    }

    for (const svg::SVGGeometryElement& geometry : CollectRenderableGeometry(selected)) {
      if (geometry == hitElement) {
        return true;
      }
    }
  }

  return false;
}

void SelectTool::onMouseDown(EditorApp& editor, const Vector2d& documentPoint,
                             MouseModifiers modifiers) {
  // Reset any in-progress drag/marquee — a previous mouse-down without
  // a matching mouse-up means the user dragged off the window or a
  // tool switch happened mid-drag. Either way, abandon the old gesture
  // silently.
  dragState_.reset();
  marqueeState_.reset();
  const bool hadSelectionAtMouseDown = editor.hasSelection();

  const auto isGraphicsElement = [](const svg::SVGElement& element) {
    return element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
      return element.isa<svg::SVGGraphicsElement>();
    });
  };
  const auto makeDragParticipant = [](const svg::SVGElement& element) {
    const svg::SVGGraphicsElement graphicsElement =
        element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
          return element.cast<svg::SVGGraphicsElement>();
        });
    const Transform2d startTransform = graphicsElement.transform();
    const std::optional<RcString> sourceTransformAttributeValue =
        element.withReadAccess([&element](svg::DocumentReadAccess&, EntityHandle) {
          return element.getAttribute("transform");
        });
    return PerElementDrag{
        .element = element,
        .startTransform = startTransform,
        .currentTransform = startTransform,
        .writebackTarget = captureAttributeWritebackTarget(element),
        .sourceTransformAttributeValue = sourceTransformAttributeValue,
    };
  };

  const auto selectedBoundsDoc =
      SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(editor.selectedElements()));
  if (!selectedBoundsDoc.empty() && !editor.selectedElements().empty()) {
    const SelectionTransformHandleIntent handleIntent = HitTestSelectionTransformHandles(
        selectedBoundsDoc, documentPoint, modifiers.pixelsPerDocUnit,
        /*includeRotate=*/!modifiers.shift);
    if (handleIntent.kind != SelectionTransformHandleKind::None) {
      const auto& selection = editor.selectedElements();
      const bool allGraphics = std::all_of(selection.begin(), selection.end(), isGraphicsElement);
      if (allGraphics) {
        std::vector<PerElementDrag> extras;
        extras.reserve(selection.size() - 1);
        for (std::size_t i = 1; i < selection.size(); ++i) {
          extras.push_back(makeDragParticipant(selection[i]));
        }

        const Box2d startBounds = CombinedSelectionBounds(selectedBoundsDoc);
        const Vector2d center = CenterOf(startBounds);
        dragState_ = DragState{
            .primary = makeDragParticipant(selection.front()),
            .extras = std::move(extras),
            .gestureKind = handleIntent.kind == SelectionTransformHandleKind::Resize
                               ? DragState::GestureKind::Resize
                               : DragState::GestureKind::Rotate,
            .corner = handleIntent.corner,
            .startDocumentPoint = documentPoint,
            .startBoundsDoc = startBounds,
            .centerDocumentPoint = center,
            .startAngleRadians = AngleFromCenter(center, documentPoint),
            .generation = nextDragGeneration_++,
        };
        return;
      }
    }
  }

  auto hit = editor.hitTest(documentPoint);

  // Snapshot-safe fallback for clicks inside the selected element's
  // bbox that miss its geometric path: clicking on the transparent
  // interior of a `<g filter>`, between strokes, etc. Only fires when
  // hitTest returned null AND not shift — when hitTest DOES return a
  // hit, the click might be on a different element (deselect /
  // re-select); we never want to hijack that.
  //
  // The race-safe variant (`tryStartRedragOnSelected`) is exposed
  // publicly so EditorShell can run it before `!isBusy()` for the
  // mid-render re-drag case — but on this code path the caller has
  // already gated on `!isBusy()`, so a live `SnapshotSelectionWorldBounds`
  // call is race-free.
  if (!hit.has_value()) {
    const auto liveBoundsDoc =
        SnapshotSelectionWorldBounds(std::span<const svg::SVGElement>(editor.selectedElements()));
    if (tryStartRedragOnSelected(editor, documentPoint, modifiers, liveBoundsDoc)) {
      return;
    }
  }

  // Click on empty space → start a marquee when there is no active
  // selection, or when Shift makes the marquee additive. A plain miss
  // with an active selection clears the selection but does not switch
  // into marquee mode; selected-object gestures should default to drag.
  if (!hit.has_value()) {
    if (!modifiers.shift) {
      // Plain click on empty space clears the selection up-front so
      // the user gets immediate visual feedback even if the drag
      // never grows past zero pixels (i.e. a quick miss-click).
      editor.clearSelection();
    }
    if (hadSelectionAtMouseDown && !modifiers.shift) {
      return;
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
  // Auto-expansion to sibling composite layers was explicitly vetoed
  // (issue #582 follow-up): if a document author wants siblings to
  // move together they wrap them in a `<g>`; the editor shouldn't
  // guess.
  //
  // TODO: double-click "focus into" to descend one level deeper in a
  // filter-group — select the clicked path instead of the whole group.
  // Requires double-click detection in the pointer-event protocol.
  svg::SVGDocument& doc = editor.document().document();
  const svg::SVGElement element = doc.withReadAccess([&](svg::DocumentReadAccess&) {
    const svg::SVGElement container = DeepestWrappingContainer(doc.svgElement());
    const svg::SVGElement topLevel = TopLevelAncestor(*hit, container);
    return HasCompositingAttribute(topLevel) ? topLevel : *hit;
  });

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

  std::vector<Box2d> dragStartBoundsDoc;
  std::vector<PerElementDrag> extras;
  if (isMultiDrag) {
    dragStartBoundsDoc.assign(selectedBoundsDoc.begin(), selectedBoundsDoc.end());
    extras.reserve(currentSelection.size() - 1);
    for (const svg::SVGElement& selected : currentSelection) {
      if (selected == element) {
        continue;  // primary handled separately
      }
      extras.push_back(makeDragParticipant(selected));
    }
  } else {
    const std::array<svg::SVGElement, 1> startSelection{element};
    dragStartBoundsDoc = SnapshotSelectionWorldBounds(startSelection);
    editor.setSelection(element);
  }

  dragState_ = DragState{
      .primary = makeDragParticipant(element),
      .extras = std::move(extras),
      .startDocumentPoint = documentPoint,
      .startBoundsDoc = CombinedSelectionBounds(dragStartBoundsDoc),
      .generation = nextDragGeneration_++,
  };
}

void SelectTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld) {
  onMouseMove(editor, documentPoint, buttonHeld, MouseModifiers{});
}

void SelectTool::onMouseMove(EditorApp& editor, const Vector2d& documentPoint, bool buttonHeld,
                             MouseModifiers modifiers) {
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
  if (!dragState_->hasMoved && deltaDoc.lengthSquared() < kDragThresholdSq) {
    return;
  }

  std::optional<Transform2d> documentFromStartDocument;
  switch (dragState_->gestureKind) {
    case DragState::GestureKind::Move:
      // Donner's Transform2 uses row-vector post-multiply semantics, so
      // `start * Translate(delta)` applies the element's existing transform
      // first and then moves the resulting document-space geometry by delta.
      documentFromStartDocument = Transform2d::Translate(deltaDoc);
      break;
    case DragState::GestureKind::Resize:
      documentFromStartDocument = ResizeTransform(dragState_->startBoundsDoc, dragState_->corner,
                                                  documentPoint, modifiers.shift, modifiers.option);
      break;
    case DragState::GestureKind::Rotate:
      documentFromStartDocument = RotateTransform(dragState_->centerDocumentPoint,
                                                  dragState_->startAngleRadians, documentPoint);
      break;
  }
  if (!documentFromStartDocument.has_value()) {
    return;
  }

  dragState_->currentDocumentDelta = deltaDoc;
  dragState_->currentDocumentFromStartDocument = *documentFromStartDocument;
  const Transform2d primaryNewTransform =
      dragState_->primary.startTransform * *documentFromStartDocument;
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
    extra.currentTransform = extra.startTransform * *documentFromStartDocument;
    editor.applyMutation(EditorCommand::SetTransformCommand(extra.element, extra.currentTransform));
  }
}

void SelectTool::beginMarquee(EditorApp& editor, const Vector2d& documentPoint, bool additive) {
  dragState_.reset();
  marqueeState_.reset();
  if (editor.hasSelection() && !additive) {
    editor.clearSelection();
    return;
  }
  if (!additive) {
    editor.clearSelection();
  }
  marqueeState_ = MarqueeState{
      .startDocumentPoint = documentPoint,
      .currentDocumentPoint = documentPoint,
      .additive = additive,
  };
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
    const bool multiElement = !dragState_->extras.empty();
    const char* undoLabel = multiElement ? "Move elements" : "Move element";
    switch (dragState_->gestureKind) {
      case DragState::GestureKind::Move:
        undoLabel = multiElement ? "Move elements" : "Move element";
        break;
      case DragState::GestureKind::Resize:
        undoLabel = multiElement ? "Resize elements" : "Resize element";
        break;
      case DragState::GestureKind::Rotate:
        undoLabel = multiElement ? "Rotate elements" : "Rotate element";
        break;
    }
    UndoSnapshot before{
        .element = dragState_->primary.element,
        .transform = dragState_->primary.startTransform,
        .writebackTarget = dragState_->primary.writebackTarget,
        .sourceTransformAttributeValue = dragState_->primary.sourceTransformAttributeValue,
        .restoreSourceTransformAttributeValue = true};
    UndoSnapshot after{.element = dragState_->primary.element,
                       .transform = dragState_->primary.currentTransform,
                       .writebackTarget = dragState_->primary.writebackTarget};
    before.extras.reserve(dragState_->extras.size());
    after.extras.reserve(dragState_->extras.size());
    for (const auto& extra : dragState_->extras) {
      before.extras.push_back(
          UndoSnapshot{.element = extra.element,
                       .transform = extra.startTransform,
                       .writebackTarget = extra.writebackTarget,
                       .sourceTransformAttributeValue = extra.sourceTransformAttributeValue,
                       .restoreSourceTransformAttributeValue = true});
      after.extras.push_back(UndoSnapshot{
          .element = extra.element,
          .transform = extra.currentTransform,
          .writebackTarget = extra.writebackTarget,
      });
    }
    editor.undoTimeline().record(undoLabel, std::move(before), std::move(after));

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
  if (!dragState_.has_value()) {
    return std::nullopt;
  }

  std::vector<Entity> extraEntities;
  extraEntities.reserve(dragState_->extras.size());
  for (const PerElementDrag& extra : dragState_->extras) {
    extraEntities.push_back(extra.element.unsafeEntityHandle().entity());
  }

  return ActiveDragPreview{
      .entity = dragState_->primary.element.unsafeEntityHandle().entity(),
      .extraEntities = std::move(extraEntities),
      .translation = dragState_->currentDocumentDelta,
      .documentFromCachedDocument = dragState_->currentDocumentFromStartDocument,
      .dragGeneration = dragState_->generation};
}

std::optional<SelectTool::ActiveGesturePreview> SelectTool::activeGesturePreview() const {
  if (!dragState_.has_value()) {
    return std::nullopt;
  }

  ActiveGestureKind kind = ActiveGestureKind::Move;
  switch (dragState_->gestureKind) {
    case DragState::GestureKind::Move: kind = ActiveGestureKind::Move; break;
    case DragState::GestureKind::Resize: kind = ActiveGestureKind::Resize; break;
    case DragState::GestureKind::Rotate: kind = ActiveGestureKind::Rotate; break;
  }

  return ActiveGesturePreview{
      .kind = kind,
      .corner = dragState_->corner,
      .startBoundsDoc = dragState_->startBoundsDoc,
      .documentFromStartDocument = dragState_->currentDocumentFromStartDocument,
      .currentDocumentDelta = dragState_->currentDocumentDelta,
      .hasMoved = dragState_->hasMoved,
  };
}

std::optional<SelectTool::ActiveTransformBoundsPreview> SelectTool::activeTransformBoundsPreview()
    const {
  if (!dragState_.has_value() || !dragState_->hasMoved ||
      dragState_->gestureKind != DragState::GestureKind::Rotate) {
    return std::nullopt;
  }

  return ActiveTransformBoundsPreview{
      .startBoundsDoc = dragState_->startBoundsDoc,
      .documentFromStartDocument = dragState_->currentDocumentFromStartDocument,
  };
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
