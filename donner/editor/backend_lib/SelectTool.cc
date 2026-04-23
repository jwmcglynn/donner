#include "donner/editor/backend_lib/SelectTool.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/editor/backend_lib/AttributeWriteback.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/backend_lib/EditorCommand.h"
#include "donner/editor/backend_lib/UndoTimeline.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {

namespace {

bool HasCompositingBoundaryAttribute(const svg::SVGElement& element) {
  return element.hasAttribute(xml::XMLQualifiedNameRef("filter")) ||
         element.hasAttribute(xml::XMLQualifiedNameRef("mask")) ||
         element.hasAttribute(xml::XMLQualifiedNameRef("clip-path"));
}

template <typename Visitor>
void ForEachGeometryDescendant(const svg::SVGElement& node, Visitor& visit) {
  if (node.isa<svg::SVGGeometryElement>()) {
    visit(node.cast<svg::SVGGeometryElement>());
  }
  for (auto child = node.firstChild(); child.has_value(); child = child->nextSibling()) {
    ForEachGeometryDescendant(*child, visit);
  }
}

/// Union of `worldBounds()` across every `SVGGeometryElement` descendant
/// of @p root (including the root itself). Mirrors
/// `SnapshotSelectionWorldBounds`'s single-element case without pulling
/// that whole translation unit in. Returns `nullopt` when the subtree
/// carries no rendered geometry.
std::optional<Box2d> SubtreeWorldBounds(const svg::SVGElement& root) {
  std::optional<Box2d> merged;
  auto visit = [&merged](const svg::SVGGeometryElement& geometry) {
    if (const auto wb = geometry.worldBounds(); wb.has_value()) {
      merged = merged.has_value() ? Box2d::Union(*merged, *wb) : *wb;
    }
  };
  ForEachGeometryDescendant(root, visit);
  return merged;
}

/// Fraction of @p inner's area that lies inside @p outer (0.0–1.0). 0 when
/// `inner` is empty or degenerate. Used as the "visual composite containment"
/// score — a sibling whose geometry is mostly inside a clicked filter-g's
/// bbox is almost certainly part of the same visual object (e.g. a bright
/// lightning-bolt core painted under its dark filter halo). Callers treat
/// values near 1.0 as "include in the drag group".
double ContainmentFraction(const Box2d& inner, const Box2d& outer) {
  const double innerArea = std::max(0.0, inner.width()) * std::max(0.0, inner.height());
  if (innerArea <= 0.0) return 0.0;

  const double ixMin = std::max(inner.topLeft.x, outer.topLeft.x);
  const double iyMin = std::max(inner.topLeft.y, outer.topLeft.y);
  const double ixMax = std::min(inner.bottomRight.x, outer.bottomRight.x);
  const double iyMax = std::min(inner.bottomRight.y, outer.bottomRight.y);
  const double iw = std::max(0.0, ixMax - ixMin);
  const double ih = std::max(0.0, iyMax - iyMin);
  return (iw * ih) / innerArea;
}

/// Collect direct siblings of @p compositingGroup whose geometry is mostly
/// contained within @p compositingGroup's bbox — the heuristic for "in the
/// same visual composite, should move together on drag".
///
/// Why this matters (issue #582): a filter group's visual contribution is
/// often painted across several SIBLING DOM nodes, not descendants. The
/// splash's small lightning bolt is three siblings under `<g class="cls-94">`:
/// `Lightning_glow_dark` (`<g filter>` — dark halo), `Lightning_glow_bright`
/// (`<g filter>` — cyan inner glow), and `Lightning_white` (no filter —
/// bright core). Dragging any one of them via the compositor-aware elevation
/// moves only that one element; the other two stay at the pre-drag position
/// and the composite visibly shears into two pieces ("the filter
/// disappeared"). Collecting overlapping siblings and dragging the set in
/// lockstep keeps the composite together.
///
/// Uses bbox containment, not just intersection, so we DON'T sweep in
/// siblings that merely happen to share a row/column with the clicked group
/// (DONNER text, Donner_line decorative squiggle). Only siblings whose
/// geometry lives mostly INSIDE the clicked group's footprint count as part
/// of the same composite. Threshold 0.70: three layers that paint the same
/// bolt naturally share ≥90 % of their footprint; a coincidental intersection
/// like a wide text path crossing the bolt's bbox falls well under 70 %.
std::vector<svg::SVGElement> CollectCompositePeerSiblings(
    const svg::SVGElement& compositingGroup) {
  std::vector<svg::SVGElement> peers;
  auto parent = compositingGroup.parentElement();
  if (!parent.has_value()) return peers;

  const auto anchorBounds = SubtreeWorldBounds(compositingGroup);
  if (!anchorBounds.has_value() || anchorBounds->isEmpty()) return peers;

  constexpr double kContainmentThreshold = 0.70;
  for (auto sibling = parent->firstChild(); sibling.has_value(); sibling = sibling->nextSibling()) {
    if (*sibling == compositingGroup) continue;

    // Refuse to expand across a compositing-boundary sibling that
    // DOESN'T belong to this composite (e.g. `Big_lightning_glow` when
    // the user clicked the small bolt's `Lightning_glow_dark`). Its
    // own filter's output lives in its own cached layer and a
    // lockstep drag would move two unrelated composites at once.
    // Containment check above already rejects non-overlapping siblings,
    // so this extra gate only blocks NEAR siblings that happen to
    // barely pass the containment threshold.
    const auto siblingBounds = SubtreeWorldBounds(*sibling);
    if (!siblingBounds.has_value() || siblingBounds->isEmpty()) continue;
    if (ContainmentFraction(*siblingBounds, *anchorBounds) < kContainmentThreshold) {
      continue;
    }
    peers.push_back(*sibling);
  }
  return peers;
}

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
    if (HasCompositingBoundaryAttribute(*parent)) {
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
  const bool didElevate = !(element == *hit);

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

  // Composite-peer expansion. When elevation moved the drag target to a
  // `<g filter>` / `<g mask>` / `<g clip-path>`, check the group's
  // siblings for ones whose geometry is mostly contained inside the
  // group's bbox — those are the other DOM nodes that visually paint
  // the same object (e.g. the splash's small lightning bolt is three
  // siblings: dark halo, cyan inner glow, bright white core — only the
  // first two are `<g filter>`; the third is a plain `<g>`). Without
  // this expansion dragging one shears the composite apart, which
  // users read as "the filter disappeared" (issue #582).
  //
  // Skipped on multi-drag (the user already curated a multi-selection,
  // don't override it) and when elevation didn't happen (click landed
  // on a leaf with no filter/mask/clip-path ancestor — expanding
  // there would trap unrelated siblings into every single-element
  // drag).
  std::vector<svg::SVGElement> compositePeers;
  if (didElevate && !isMultiDrag) {
    compositePeers = CollectCompositePeerSiblings(element);
  }

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
  } else if (!compositePeers.empty()) {
    // Composite drag: primary = the elevated filter-g, extras = its
    // overlapping sibling layers. Set the editor's selection to the
    // full set so overlay chrome draws AABBs around every peer and
    // downstream panels (tree view, inspector) see a coherent group.
    std::vector<svg::SVGElement> fullSelection;
    fullSelection.reserve(1 + compositePeers.size());
    fullSelection.push_back(element);
    for (const auto& peer : compositePeers) {
      fullSelection.push_back(peer);
    }
    editor.setSelection(std::move(fullSelection));
    extras.reserve(compositePeers.size());
    for (const auto& peer : compositePeers) {
      const Transform2d peerStart = peer.cast<svg::SVGGraphicsElement>().transform();
      extras.push_back(PerElementDrag{
          .element = peer,
          .startTransform = peerStart,
          .currentTransform = peerStart,
          .writebackTarget = captureAttributeWritebackTarget(peer),
          .sourceTransformAttributeValue = peer.getAttribute("transform"),
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
