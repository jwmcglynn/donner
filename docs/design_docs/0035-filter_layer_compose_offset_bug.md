## Design: Filter Layer Compose-Offset Bug — Analysis & Bisection Plan

**Status:** Investigation (no fix landed)
**Author:** Claude Opus 4.7 (1M context)
**Created:** 2026-05-14

## Summary

After moving a non-filtered shape (e.g. a polygon letter in `donner_splash.svg`'s
`<g id="Donner">` group) and then moving a filtered shape (e.g.
`<g id="Lightning_glow_dark" filter="url(...)">`), the editor's compose
displays *both* shapes at their pre-drag positions — even though the live DOM
holds the post-drag transforms and the source-pane text has been updated by
the classifier-path writeback. The user-visible symptom the operator reported
on `donner_splash.svg` at retina (`dpr=2`) is "filter shape jumps to a new
position, as if the mouse move was done at the new zoom level" — the
"2× translation multiplier" interpretation. Ground-truth rendering of the
final source bytes shows the shapes at their correct post-drag positions, so
the bug lives in the compositor's *state preservation across the drag
lifecycle* (promote → drag-frames → mouse-up → writeback → hysteresis
demote), not in the source/DOM mutation or in the basic compose math.

This doc captures everything we've established about the bug, every fix
attempt that didn't land, and the bisection plan a future investigator can
pick up without re-running the diagnostic cycle from scratch. A red-but-
disabled `.rnr` replay test (`RnrReplayTest::DISABLED_FilterPostDragJump
ReplayMatchesGroundTruth`) is committed alongside this doc and reproduces
the bug deterministically with a 39,406-pixel diff against ground-truth-
from-source.

## Goals

1. **Pin the failure surface** with a deterministic `.rnr` replay test that
   reproduces the wrong-pixels output against a known-good ground-truth.
   ✅ landed (disabled pending fix).
2. **Identify the exact compositor state divergence** that produces the
   wrong pixels — which layer/segment/`canvasFromBitmap` value is wrong on
   the failing frame.
3. **Land a fix that re-enables the test** without regressing the existing
   structural-remap optimization (the `DragEndWritebackTakesStructural
   RemapPath` and `DeleteElementDoesNotResetPreviouslyMovedShapes`
   invariants must continue to hold).

## Non-Goals

- **Re-architecting promote/demote.** The hysteresis-demote design from
  0033 §M9 (`pendingDemotions_`) is a perf optimization for drag-release ⇄
  drag-again on the same entity. The fix should respect that contract; the
  bug is somewhere downstream of the demote decision, not in the demote
  policy itself.
- **Tightening the `compositionTransformsPass` math.** Minimal tests
  confirm the math is correct for the direct `SetTransformCommand` path —
  the bug requires the full `SelectTool` drag flow.
- **Source-pane / writeback rewriting.** The source bytes, classifier
  decisions, and `SetAttributeCommand` applies are all correct
  (verified — see "What's confirmed correct" below).

## Next Steps

1. **Bisect with `BISECTION_FRAME`.** The replay harness already supports
   `BISECTION_FRAME=N` (see `RnrReplay_tests.cc` `ParseBisectFrame`); the
   `.rnr` has 288 frames. Binary-search for the first frame whose captured
   bitmap diverges from a per-frame ground truth rendered against the
   replay's source-at-that-frame.
2. **Instrument the failing frame.** Dump every layer's `bitmap`,
   `bitmapEntityFromWorldTransform`, `canvasFromBitmap`, `canvasOffset`,
   `isDirty`, and the segment dirty flags. Inspect by hand to find the
   stale value.
3. **Reverse-engineer the divergence.** Once we have the stale value,
   trace back through `consumeDirtyFlags` / `compositionTransformsPass` /
   `rasterizeLayer` / fast-path / `processPendingDemotions` to find the
   missing invalidation or wrong write.

## Implementation Plan

- [ ] **M1: Per-frame ground-truth bisect harness.**
  - [ ] Add a mode to `ReplayRecording` that snapshots `liveSource_`
        at each frame.
  - [ ] For each candidate frame `N`, render a ground truth from the
        source-at-frame-N and compare to the replay's bitmap-at-frame-N.
  - [ ] Binary-search to find the first failing `N`.
- [ ] **M2: Compositor-state snapshot at failing frame.**
  - [ ] Expose `CompositorController::snapshotLayerStateForTesting()`
        returning every layer's `bitmap` content (not just thumbnail),
        `bitmapEntityFromWorldTransform`, `canvasFromBitmap`,
        `canvasOffset`, `isDirty`, plus segment dirty flags and
        `staticSegmentBoundaries_`.
  - [ ] Dump it at frame `N-1` (last good), frame `N` (first bad),
        and frame `N+1` (steady-state bad).
- [ ] **M3: Identify and fix.**
  - [ ] Diff the three snapshots from M2; locate the field that's
        stale or wrong at frame `N`.
  - [ ] Trace its write/clear sites in `CompositorController` and
        identify the missing invalidation.
  - [ ] Land the fix, flip the test from `DISABLED_` to `TEST_F`,
        verify red-on-parent → green-on-fix.

## Background

The bug surfaced during the design-doc-0033 / 0034 work on editor
responsiveness for `donner_splash.svg` at retina-zoom. The full sequence
of related fixes already landed:

- 0033 §M9 — layer-set hysteresis (`pendingDemotions_`).
- Commit 67ca2e19 — keep `compositorInteractionKind_` at `ActiveDrag`
  across mouse-up so `composeLayers`'s `skipMainCompose` stays engaged
  for drag-release ⇄ drag-again.
- Commit (delete-flash fix) — `demoteEntity` immediately evicts orphan
  hints when the entity is no longer in the live tree.

After all that landed, the operator reported residual visual glitches:
the filter shape and any previously-dragged non-filtered shape appear at
the *wrong* position after the drag sequence. The user's
characterization — "moves twice as much as it should" — fits the case
where the layer's cached bitmap shifts via `canvasFromBitmap`
*and* the bitmap content is *already* at the post-drag position (so
`canvasFromBitmap` shifts it a second time). But static analysis of
`setBitmap` (which resets `canvasFromBitmap = Identity`) plus
`compositionTransformsPass` (which sets `canvasFromBitmap = stamp.inv *
current`) doesn't show a path that produces that double-shift state.
The `.rnr` confirms the bug is real; the root cause is still open.

## What's Confirmed Correct

These were verified during the investigation and can be eliminated from
the suspect list:

1. **The DOM is correct.** At the end of the `.rnr` replay,
   `polygon.transform = Translate(19, 11)` and
   `Lightning_glow_dark.transform = Translate(26, 3)`. Confirmed by
   walking the live `SVGElement` tree.
2. **The source bytes are correct.** `liveSource_` after replay
   contains `transform="translate(19, 11)"` on the cls-81 polygon and
   `transform="translate(26, 3)"` on `<g id="Lightning_glow_dark">`.
   Confirmed by dumping `liveSource_` and grepping.
3. **Ground-truth-from-source renders correctly.** Loading
   `liveSource_` into a fresh `EditorApp` and rendering once produces
   the expected post-drag visual (polygon shifted right, filter bolt
   shifted, "DON NER" with the visible gap).
4. **`compositionTransformsPass` math is correct.** Instrumented to
   print `stamp.t / cur.t / canvasFromBitmap.t`. For
   `Lightning_glow_dark`: `stamp.t=(0,0)`, `cur.t=(52,6)`,
   `canvasFromBitmap.t=(52,6)` — exactly what the compose needs to
   shift the cold-load bitmap to the post-drag position.
5. **`isTranslation()` on the delta returns true.** The delta
   composition `Scale(2).inv * (Translate(26,3) * Scale(2))` collapses
   to `Translate(52, 6)` in canvas pixels — pure translation, so the
   fast path's `setCanvasFromBitmap(delta)` does fire.
6. **Direct `SetTransformCommand` paths work.** Two minimal regression
   tests (`MinimalFilterDragViaSetTransformCommand` and
   `MinimalPolygonThenFilterDragViaSetTransformCommand`, removed after
   triage but described here for posterity) render correctly:
   ```cpp
   app.applyMutation(EditorCommand::SetTransformCommand(*filter,
       Transform2d::Translate({26.0, 3.0})));
   app.flushFrame();
   // Render → matches ground-truth byte-for-byte.
   ```
   Both single-shape and two-shape sequences pass. The bug requires the
   full `SelectTool` drag flow.

## What's Suspect

These are the paths that *do* fire in the failing `.rnr` replay but
*don't* fire in the passing minimal tests:

### S1. `SelectTool`'s drag promotion via `dragPreview`

During the drag, `SelectTool.onMouseMove` populates
`dragState_.primary.element` and `activeDragPreview()` returns a non-
nullopt preview. The `AsyncRenderer` worker reads
`request.dragPreview->entity` and calls `compositor_->promoteEntity(
entity, ActiveDrag)`. This makes the dragged entity (the polygon) into
its *own* compositor layer.

Implication: during the polygon's drag, the layer set has *8* layers
(the 7 mandatory/bucket layers plus the polygon as its own layer).
Surrounding segments are *carved* to exclude the polygon's paint-order
range, so when the polygon was carved out, the cached segments around
its range no longer contain the polygon's pixels.

### S2. Hysteresis-demote after the user clicks the filter

When the user clicks `Lightning_glow_dark`, the worker's
`entityChanged` branch fires `compositor_->demoteEntity(polygon)`.
`demoteEntity` doesn't immediately remove the hint — it queues a
30-frame hysteresis (`pendingDemotions_[polygon] =
kDemotionHysteresisFrames`). The polygon's layer stays in `layers_`
during the hysteresis window.

After 30 frames, `processPendingDemotions` removes the hint, runs
`resolver_.resolve` + `reconcileLayers`, and the polygon's layer
disappears from `layers_`. The surrounding segments around the
polygon's range *merge* back into one slot whose paint-order range
once again *includes* the polygon's entity.

The merged slot's *boundary identity pair* `(layer_before, layer_after)`
differs from either of the pre-demote pairs (`(layer_before, polygon)`
and `(polygon, layer_after)`), so `resyncSegmentsToLayerSet`'s
boundary-identity match in `staticSegmentBoundaries_` fails for the
merged slot and `newDirty[merged] = true`. `rasterizeDirtyStaticSegments`
re-rasterizes the slot using current RICs — which should produce a
bitmap with the polygon at its post-drag transform.

Tested: forcing `markAllSegmentsDirty()` at the end of
`processPendingDemotions` doesn't change the failing pixels. So the
segment dirty flag *is* being set; the segment *is* re-rasterizing.
The bug is elsewhere.

### S3. `setAttribute("transform", "...")`'s no-op + dirty-flag side effect

`DocumentSyncController::applyPendingWritebacks` → `QueueSourceWriteback
Reparse` → `classifyTextChange` emits a `SetAttributeCommand` for a
single-attribute insertion. `AsyncSVGDocument::applyOne` calls
`element.setAttribute("transform", "translate(...)")`. Inside
`SVGElement::trySetPresentationAttribute`, the `Parse` function checks
`params.specificity < destination->specificity` and bails because the
existing `Property` was set at `Specificity::Override()` (from the
during-drag `SetTransformCommand`) and the new value comes in at
presentation-attribute specificity. The `Property` is *not* updated.

But `trySetPresentationAttribute` *also* calls `markDirty(handle_,
Transform | WorldTransform | RenderInstance)` and
`propagateWorldTransformDirtyToDescendants(handle_)` — regardless of
whether the `Property` changed. So the entity (and its descendants)
gets `DirtyFlagsComponent` set even though the transform value is
identical to what it already was.

This dirty-flag side effect drives `consumeDirtyFlags` to mark the
containing layer dirty on the *next* renderFrame, which fires a layer
re-rasterize. The re-rasterize captures the current RIC's
`worldFromEntityTransform` into the new stamp — which *should* update
`bitmapStamp` to the post-drag transform.

If this side-effect rasterize *happens* but for some reason captures
the *pre*-drag transform into the stamp (e.g., the RIC wasn't rebuilt
between the `SetTransformCommand` and this rasterize), then
`compositionTransformsPass` on the next frame computes
`delta = stamp.inv * current` against a stamp that has the same
translation as `current` — `delta = Identity`,
`canvasFromBitmap = Identity` — *but the bitmap contents already have
the post-drag shape baked in*. Visual: shape at post-drag position via
the rasterized bitmap, with no compose offset. ✓ — this matches one
interpretation of the bug.

Or the inverse: rasterize captures the post-drag stamp,
`canvasFromBitmap = Identity` ✓, the bitmap has the shape at the
post-drag transform; but `compositionTransformsPass` runs *next*
frame against a `cur.t` that has *moved further* (because something
re-applied the transform). delta is non-zero;
`canvasFromBitmap = Translate(delta)`; visual = (bitmap post-drag) +
(extra translate) = 2× drag. ✓ — also matches.

### S4. `rasterizeCount=4` for every layer

The compositor's layer inspector reports `rasterizeCount=4` for every
layer at the end of the replay, even ones the user never touched. Cold
load = 1; the remaining 3 are unexplained. Likely candidates:
- Canvas-size changes from `.rnr`-recorded viewport zoom updates
  trigger `setCanvasSize` → `invalidateRenderTree` →
  `needsFullRebuild` → `rootDirty_=true` → all layers re-rasterize.
- If 3 zoom commits happen during the recording, that's 3 extra
  rasterizes per layer.

Each rasterize captures `bitmapEntityFromWorldTransform = current RIC's
worldFromEntityTransform`. If the *last* rasterize for
`Lightning_glow_dark` happens *before* the user's drag, the stamp is
the pre-drag value (`canvasFromDoc * Identity`). The instrumentation
confirms this: `stamp.t = (0, 0)` at end of replay even though
`cur.t = (52, 6)`.

So the cached bitmap has the *pre-drag* filter content. The compose
*should* shift it by `canvasFromBitmap = Translate(52, 6)` to land at
the post-drag position. Empirically it doesn't — but the math says it
should. The disconnect between the math and the bitmap is the
remaining mystery.

## Fix Attempts That Didn't Work

Documented so a future investigator doesn't retread:

### A1. Fall through to `resetAllLayers` on any parser-dirty entity

```cpp
// In remapAfterStructuralReplace, after capturing parserDirtyEntities:
if (!parserDirtyEntities.empty()) {
  return false;  // AsyncRenderer falls back to resetAllLayers.
}
```

**Result:** Fixes the .rnr replay (full reset rebuilds every cache from
scratch with current state). **Regresses
`AsyncRendererE2ETest::DragEndWritebackTakesStructuralRemapPath` and
`RnrReplayTest::DeleteElementDoesNotResetPreviouslyMovedShapes`**, both
of which require the preservation path. Tradeoff is unacceptable —
ANY drag-end writeback always has parser-dirty entities, so this
disables the optimization entirely.

### A2. Mark all segments dirty in `processPendingDemotions`

```cpp
void CompositorController::processPendingDemotions(Registry& registry) {
  ...
  resolver_.resolve(...);
  reconcileLayers(registry);
  markAllSegmentsDirty();  // ← added
}
```

**Result:** No change to failing-pixel count (still 39,406 px diff).
Either the segments *are* already being marked dirty (via
`resyncSegmentsToLayerSet`'s `newSegments[i].empty()` path on
boundary-identity mismatch) and this is redundant, or the segment
rasterize is happening but the *layer* is the one with stale content.

### A3. Capture pre-prepare dirty entities and route through `consumeDirtyFlags` post-remap

```cpp
// In remapAfterStructuralReplace, before prepareDocumentForRendering:
std::vector<Entity> parserDirtyEntities;
{
  auto dirtyView = registry.view<components::DirtyFlagsComponent>();
  for (const Entity entity : dirtyView) {
    if (dirtyView.get<DirtyFlagsComponent>(entity).flags != Flags::None) {
      parserDirtyEntities.push_back(entity);
    }
  }
}
// ... existing prepare + remap ...
// At end:
consumeDirtyFlags(parserDirtyEntities);  // ← added
for (auto& segment : staticSegments_) {
  segment = RendererBitmap{};
}
```

**Result:** Marks 8/8 layers dirty (confirmed via instrumentation),
clears every cached segment bitmap, forces every rasterize to fire.
**Still 39,406-pixel diff.** Either the post-prepare rasterizes pick
up stale RICs, or the bug isn't in the layer/segment bitmaps —
it's in `canvasFromBitmap` or compose order.

### A4. Clear `staticSegments_` bitmap contents in `remapAfterStructuralReplace`

```cpp
for (auto& segment : staticSegments_) {
  segment = RendererBitmap{};
}
```

**Result:** Same pixel count. `resyncSegmentsToLayerSet` would have
preserved the bitmap via boundary-identity matching otherwise, but
that path was *already* failing to match (different layer entity IDs
after remap), so the clear is a no-op.

## Reproduction Recipe

```bash
# Run the disabled test (writes diagnostic PNGs to TEST_UNDECLARED_OUTPUTS_DIR).
bazel test //donner/editor/tests:rnr_replay_tests \
    --test_filter='RnrReplayTest.DISABLED_FilterPostDragJumpReplayMatchesGroundTruth' \
    --test_output=streamed --cache_test_results=no \
    --test_arg=--gtest_also_run_disabled_tests

# Diagnostic PNGs in the test outputs dir:
#   filter_post_drag_jump_01_replay_final.png    — replay's final bitmap (BUG)
#   filter_post_drag_jump_02_ground_truth.png    — fresh load of liveSource_ (correct)
#   diff_filter_post_drag_jump_replay_vs_ground_truth.png  — pixel diff
#   side_by_side_*.png                            — actual + expected side-by-side
```

The recording `donner/editor/tests/filter_post_drag_jump.rnr` is 288 frames:
1. Click polygon (cls-81 / second N of "DONNER").
2. Drag polygon by `translate(19, 11)`.
3. Mouse-up.
4. Click `Lightning_glow_dark` (the filter group at the bottom of the
   splash).
5. Drag `Lightning_glow_dark` by `translate(26, 3)`.
6. Mouse-up.
7. (Replay's settle frames — no more user input.)

After replay: `compositorReconstructCount=1` (no full reset),
`layers_.size() == 7`, polygon is *not* a layer (demoted via
hysteresis), filter (`entity=672, id=Lightning_glow_dark`) is layer #2
with `bitmapSize=1784×1024, canvasOffset=(0,0)`.

Both shapes render at their *pre-drag* canvas positions in the replay's
bitmap; ground-truth-from-source shows them at the *post-drag*
positions. 39,406 px diff (vs the 2,000-px threshold).

## Proposed Architecture

The investigation has narrowed the bug to one of:

1. **A stale `canvasFromBitmap`** that's been written by some path other
   than `compositionTransformsPass` and not reset by the post-rasterize
   `setBitmap`. Audit every write to `canvasFromBitmap_` /
   `setCanvasFromBitmap` and verify the order vs `setBitmap`.
2. **A wrong `bitmapEntityFromWorldTransform` stamp** captured at the
   wrong moment. The rasterize captures `worldFromEntity` from the
   *current* RIC, which is the RIC at *that frame*. If a frame's
   rasterize happens *between* a `SetTransformCommand` mutation and the
   `prepareDocumentForRendering` RIC rebuild — or if the fast path's
   `instance.worldFromEntityTransform = res.newWorldFromEntity`
   (`CompositorController.cc:1200`) writes a value that's then *not*
   what the rasterize captures — the stamp can drift.
3. **A segment paint-order range bug** post-demote. When the polygon's
   layer is removed, the merged segment's `[firstIdx, lastIdx]` in
   `paintOrder` may include or exclude the polygon's RIC depending on
   how `rasterizeDirtyStaticSegments` recomputes layer ranges. If it
   excludes the polygon, the merged segment doesn't draw the polygon at
   all — but the polygon's layer was just removed, so no bitmap is
   drawing it either. The compose would have a transparent gap, not a
   pre-drag-position shape. ✗ doesn't match the symptom.

(1) and (2) remain plausible; (3) doesn't fit the visual.

## Testing and Validation

- `RnrReplayTest::DISABLED_FilterPostDragJumpReplayMatchesGroundTruth`
  is the pinned repro. A fix attempt:
  1. Flips it from `DISABLED_` to `TEST_F`.
  2. Verifies the test fails on the parent commit (red).
  3. Lands the fix and verifies green at the fix commit.
  4. Confirms `DragEndWritebackTakesStructuralRemapPath` +
     `DeleteElementDoesNotResetPreviouslyMovedShapes` *still pass*
     (the structural-remap optimization must survive).
- A `BISECTION_FRAME=N` mode that finds the first failing frame would
  let us pin not just "the final state is wrong" but "the state goes
  wrong at frame N when the user does X." This sharpens any fix.

## Open Questions

1. **Why does `rasterizeCount=4` for every layer, not just the filter?**
   The polygon's drag shouldn't dirty the filter's layer (different
   ranges). The 4 rasterizes per layer suggest something is forcing
   `rootDirty_` or marking-all-dirty 3 times during the replay.
   Candidates: viewport zoom commits in the `.rnr`, or some
   `needsFullRebuild` getting set by a path that isn't documented.
2. **Is the polygon's layer ever actually promoted?** If `SelectTool`'s
   `promoteEntity(polygon, ActiveDrag)` is being silently refused, the
   polygon stays in its parent's complexity-bucket layer (or in a
   static segment) and the bug story changes entirely. The
   `lastPromoteRefusalReason` / `lastPromoteRefusalEntity` accessors
   would let us verify.
3. **Does the bug also fire if the recording does *only* the polygon
   drag (no filter drag)?** A trimmed `.rnr` would isolate whether the
   filter's drag is essential to the bug or merely co-occurring.
4. **Does the bug fire at `dpr=1`?** All current repros are at retina
   (`dpr=2`). If the same recording at `dpr=1` produces correct pixels,
   the bug has a `canvasFromDoc`-scale dependency, narrowing it
   further.

## Alternatives Considered

- **Always re-rasterize on drag-end** (revert to non-preserving path).
  Correctness wins, perf loses. Rejected: this is exactly the
  optimization 0033 §M9 + the structural-remap path were designed to
  preserve. Going back means accepting multi-second freezes for every
  drag-release on a complex document.
- **Don't allow `promoteEntity` for non-mandatory entities.** Then the
  polygon stays in its bucket layer and the demote round-trip is
  avoided. But the editor uses promotion to keep drag latency
  bounded — disabling it would re-introduce the "drag of any shape at
  high zoom is multi-second" bug from before 0033.
- **Eagerly invalidate every entity dirty flag at frame boundaries.**
  Heavy hammer; would likely fix this and many other latent stale-
  state bugs but at significant per-frame cost.

## Future Work

- [ ] **Land the M1–M3 plan** to identify and fix the divergence.
- [ ] **Add a "compositor state diff" test fixture** that compares two
      snapshots of layer + segment state and reports the first
      divergent field. Reusable across future state-preservation
      regressions.
- [ ] **Document the layer-set transition lifecycle** in
      `CompositorController.h` once we understand the failure mode —
      the promote/drag/demote sequence has enough subtle invariants
      that future regressions are likely.
