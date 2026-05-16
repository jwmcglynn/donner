## Design: Filter Layer Compose-Offset Bug — Resolution and Stress Coverage

**Status:** Fixed for the 0035 replay; drag+zoom stress coverage added
**Author:** Claude Opus 4.7 (1M context)
**Created:** 2026-05-14
**Updated:** 2026-05-15

## Summary

After moving a non-filtered shape (e.g. a polygon letter in `donner_splash.svg`'s
`<g id="Donner">` group) and then moving a filtered shape (e.g.
`<g id="Lightning_glow_dark" filter="url(...)">`), the editor's compose
displayed _both_ shapes at their pre-drag positions even though the live DOM
and source-pane text held the post-drag transforms. The operator-visible
symptom on `donner_splash.svg` at retina (`dpr=2`) was "filter shape jumps to
a new position, as if the mouse move was done at the new zoom level" — the
"2× translation multiplier" interpretation.

The root cause was source sync, not compositor state preservation:
editor-owned writebacks were routed back through the structured text
classifier. That updated the live DOM redundantly but left the live
`XMLNode` source ranges at their pre-patch byte offsets, so a second
writeback against an unidentified sibling could resolve to the *next*
sibling. The most visible artifact was the second `N` in "DONNER" shifting
its neighbor.

The fix has two halves:

1. **`QueueSourceWritebackReparse` always queues a preserving reparse.** The
   classifier hot path is gone for editor-originated writebacks; the
   structured editor still handles user-authored source-pane edits.
   `setDocumentMaybeStructural` still hands the compositor a structural
   remap when the tree shape is stable, so cached layers survive.
2. **`SVGElement::trySetPresentationAttribute` invalidates the layout cache
   on `transform=` writes.** Replaces a bare
   `markDirty(Transform | WorldTransform | RenderInstance)` with
   `LayoutSystem().invalidate(handle_) + markDirty(RenderInstance)`. Without
   it, the property's specificity check could no-op the write while
   leaving `ComputedAbsoluteTransformComponent` cached at pre-change
   values.

A third surface — `setSkipMainComposeDuringSplit(activeDragRequest)` in
`AsyncRenderer` — toggles the flat-fallback skip per request rather than
once at construction. Post-drag and selection-prewarm renders now refresh
the flat bitmap; previously they could re-use the pre-drag baseline while
the DOM and tile metadata had moved on.

## Goals

1. **Pin the failure surface** with a deterministic `.rnr` replay test that
   reproduces the wrong-pixels output against a known-good ground-truth.
   ✅ landed and enabled.
2. **Refresh source locations after editor-owned writebacks** without
   treating the source-pane echo as a user edit. ✅ landed in
   `QueueSourceWritebackReparse`.
3. **Preserve the structural-remap optimization** so drag-end writebacks do
   not tear down every cached layer. ✅ covered by async renderer and replay
   tests.
4. **Add broader drag+zoom stress coverage** for the related class of layer
   shift/scale bugs and editor hangs. ✅ landed in
   `EditorLayerStressTest.DragZoomWritebackStressKeepsCompositionAlignedAndDoesNotHang`.

## Non-Goals

- **Re-architecting promote/demote.** The hysteresis-demote design from
  0033 §M9 (`pendingDemotions_`) is a perf optimization for drag-release ⇄
  drag-again on the same entity. Out of scope.
- **Disabling structured editing for user-authored source edits.** The
  classifier still handles real source-pane edits. The fix only changes
  editor-originated writebacks, which already mutated the DOM before the
  source text was patched.

## Implementation Plan

- [x] **M1: Preserve structural remap across writeback reparses.**
      `AsyncRenderer` remaps existing compositor state before falling back
      to reset on document swaps that carry a structural remap.
- [x] **M2: Refresh flat fallback after drag settle.**
      `skipMainComposeDuringSplit` is enabled only for actual `ActiveDrag`
      requests, so post-release renders refresh the flat bitmap.
- [x] **M3: Stop self-writebacks from using stale source offsets.**
      `QueueSourceWritebackReparse` always queues a preserving reparse;
      `trySetPresentationAttribute` calls `LayoutSystem().invalidate` so
      the cached computed transforms don't drift when the property write
      is a same-value no-op.
- [x] **M4: Stress editor drag+zoom composition.**
      `editor_layer_stress_tests` drives a layered SVG through repeated
      click/drag/zoom/writeback cycles and compares settled frames to
      fresh full-render references.

## Background

The bug surfaced during the 0033 / 0034 work on editor responsiveness for
`donner_splash.svg` at retina-zoom. The chain of related fixes that
landed before this one:

- 0033 §M9 — layer-set hysteresis (`pendingDemotions_`).
- Commit 67ca2e19 — keep `compositorInteractionKind_` at `ActiveDrag`
  across mouse-up so `composeLayers`'s `skipMainCompose` stays engaged
  for drag-release ⇄ drag-again.
- Delete-flash fix — `demoteEntity` immediately evicts orphan hints
  when the entity is no longer in the live tree
  (`IsEntityInLiveTree` helper).

The operator's characterization — "moves twice as much as it should" —
fit the case where the layer's cached bitmap shifts via
`canvasFromBitmap` AND the bitmap content is already at the post-drag
position. That was the wrong mental model: static analysis showed
`setBitmap` resets `canvasFromBitmap = Identity`,
`compositionTransformsPass` writes the right delta, and direct
`SetTransformCommand` paths render byte-for-byte against ground truth.
The divergence had to live somewhere `SelectTool`'s drag flow visits
that the synthetic tests don't. That somewhere was source-sync.

## Root Cause

`DocumentSyncController::applyPendingWritebacks` patches source text
after a drag-end and called `QueueSourceWritebackReparse` with both the
pre-patch and post-patch source. The old body:

```cpp
if (app.structuredEditingEnabled() && app.hasDocument()) {
  auto classified = classifyTextChange(app.document().document(),
                                       previousSourcePrePatch, newSource);
  if (classified.command.has_value()) {
    app.applyMutation(std::move(*classified.command));
    return;  // <-- live DOM only; XML source ranges left stale.
  }
}
app.applyMutation(EditorCommand::ReplaceDocumentCommand(*previousSourceText,
                                                        /*preserveUndoOnReparse=*/true));
```

Classified single-attribute insertions / modifications never re-parsed,
which meant every `XMLNode::getNodeLocation()` returned the pre-patch
byte range. The live DOM was correct; the compositor never sees the XML
ranges directly. But the *next* editor-owned writeback computed its
target offset from the stale range and wrote into the wrong span —
landing on the next sibling when neither sibling had an `id`.

Compounding it: the classifier-emitted `SetAttributeCommand` hit
`trySetPresentationAttribute`, observed that the during-drag
`SetTransformCommand` already wrote `transform` at
`Specificity::Override()`, and bailed out of the property write (lower
specificity loses). But it still called
`markDirty(Transform | WorldTransform | RenderInstance)` without
clearing the cached `ComputedAbsoluteTransformComponent` — so the next
render saw the dirty flag but the cached transform stuck at its
pre-write value. The replacement
`LayoutSystem().invalidate(handle_)` drops the computed cache as well
as marking the dirty flags.

## Tests

- `RnrReplayTest.FilterPostDragJumpReplayMatchesGroundTruth` — the
  original `.rnr` replay. Now passes against ground-truth-from-source.
- `EditorSyncTest.StructuredSelfWritebackDoesNotRetargetRepeatedUnidentifiedSiblingDrag` —
  pins the source-sync root cause. Two adjacent un-id'd siblings; the
  second writeback to the first sibling must not retarget the second.
- `SVGElementTests.TransformAttributeInvalidatesDescendantAbsoluteTransformCache` —
  unit-level cache-invalidation guarantee for the
  `trySetPresentationAttribute` change.
- `AsyncRendererE2ETest.DragEndWritebackTakesStructuralRemapPath` —
  preserving reparses still take the structural-remap path (the
  optimization survives the fix).
- `EditorLayerStressTest.DragZoomWritebackStressKeepsCompositionAlignedAndDoesNotHang` —
  click/prewarm, active drag, canvas-size zoom changes during drag,
  source writeback reparses, async cancellation / repost pressure,
  settled bitmap identity against a fresh full-render reference, and
  no async worker hang.
- `RnrReplayTest.DeleteElementDoesNotResetPreviouslyMovedShapes` — pins
  the delete-flash fix (orphan-hint eviction in `demoteEntity`).

## Reproduction Recipe

```bash
bazel test //donner/editor/tests:rnr_replay_tests \
    --test_filter='RnrReplayTest.FilterPostDragJumpReplayMatchesGroundTruth' \
    --test_output=streamed --cache_test_results=no

# On failure, diagnostic PNGs land in TEST_UNDECLARED_OUTPUTS_DIR:
#   filter_post_drag_jump_01_replay_final.png    — replay's final bitmap
#   filter_post_drag_jump_02_ground_truth.png    — fresh load of liveSource_
#   diff_*.png, side_by_side_*.png               — pixel diff + side-by-side
```

The recording `donner/editor/tests/filter_post_drag_jump.rnr` is 288
frames: click polygon (cls-81 / second N), drag by `translate(19, 11)`,
mouse-up; click `Lightning_glow_dark`, drag by `translate(26, 3)`,
mouse-up; settle.

## Open Items

The 0035 fix unblocks the primary repro but exposes two follow-on
symptoms on the same `donner_splash.svg` workload that deserve their own
design doc:

1. **Hangs when dragging and zooming.** A canvas-size commit during or
   immediately after a drag triggers `setCanvasSize` →
   `invalidateRenderTree` → `needsFullRebuild` → `rootDirty_=true` and
   the compositor re-rasterizes every promoted layer at the new canvas
   scale. At 3× retina that's ~10 MB × 7 layers of rasterize work,
   hundreds of milliseconds to multiple seconds, during which the worker
   is `isBusy` and the editor blocks new clicks. The §M2 change to
   toggle `skipMainComposeDuringSplit` per request makes this cost more
   visible post-release (compose runs every non-drag frame) without
   changing the underlying rasterize cost.

   This is the same shape of problem 0034 (progressive rendering) solved
   for *drag-start* after zoom: ship an intermediate (stretched prior
   canvas) immediately, refine in the background. A future "progressive
   rendering for canvas resizes" doc should apply the same pattern to
   mid-drag and post-drag canvas-size changes.

2. **Tearing on elevated layers — wrong scale after zoom.** Symptom:
   after a zoom, individual promoted layers appear at the wrong scale
   relative to the rest of the canvas. Most plausible cause is the §M4
   cancel-and-restart rasterize loop interacting with a canvas-scale
   change: the loop bails mid-way (new request lands), some layers
   re-rasterized at the new scale and updated their
   `bitmapEntityFromWorldTransform` stamp + `canvasOffset`, others
   still hold old-scale bitmaps. `composeLayers` blits them all
   together; mixed-scale tearing follows. The "atomic re-rasterize"
   property (all layers or none) was implicit before §M4 cancellation
   and is no longer enforced.

   Two candidate shapes for the fix:
   - Block `composeLayers` until the rasterize-all loop completes when
     `needsFullRebuild` was the trigger (atomicity on canvas-scale
     change). Re-introduces some hang but preserves visual coherence.
   - Track per-layer "this bitmap is at canvas scale S" metadata; the
     compose stretches in GL (or hides) any layer whose `S` doesn't
     match the current viewport, until its re-rasterize lands. Matches
     the 0034 progressive-rendering model.

## Alternatives Considered

- **Always re-rasterize on drag-end** (revert to non-preserving path).
  Correctness wins, perf loses. Rejected: this is the optimization 0033
  §M9 + the structural-remap path were designed to preserve. Going back
  means multi-second freezes on every drag-release of a complex document.

## Notes on the Investigation

Hypotheses that were ruled out before the source-sync mechanism
surfaced (kept brief; commit history has the long-form):

- *Compositor state preservation* across promote → drag → demote: the
  cached layer bitmaps and `canvasFromBitmap` deltas are correct
  end-to-end. Verified by direct `SetTransformCommand` tests rendering
  byte-for-byte against ground truth.
- *`compositionTransformsPass` math*: `stamp.inv * current` produces
  `Translate(52, 6)` for the filter's drag, exactly the compose offset
  needed. Verified with instrumentation in the pass.
- *Hysteresis-demote segment dirty propagation*: forcing
  `markAllSegmentsDirty()` at the end of `processPendingDemotions`
  doesn't change the failing pixels — segment re-rasterize was already
  firing.
- *`setAttribute` no-op + dirty-flag side effect*: actually *was*
  load-bearing, but only via the computed-cache drift it left behind
  (now fixed by `LayoutSystem().invalidate`). The dirty flags themselves
  were always correct.
