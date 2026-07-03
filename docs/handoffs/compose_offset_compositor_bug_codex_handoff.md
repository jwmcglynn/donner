# Codex handoff: preexisting drag/compositor defect cluster

**For:** a manual `codex` run (the deep compositor work kept killing Claude sub-agents with
spurious "usage policy" API errors mid-task; route it through codex instead).

**Goal:** make `bazel test //...` fully green with **zero disabled/skipped tests**. Per the
project's CLAUDE.md "Always-Green Main" policy there are *no preexisting issues* — every red
test here is in scope, including a segfault. "Done" = full green via real production fixes,
never by weakening a test.

---

## Where to work

- **Worktree:** `/Users/jwm/Projects/donner/.claude/worktrees/fixdrag`
- **Branch:** `v08/fix-drag-compose`, **HEAD `774b6ff1`** (2 commits above base `c0e8c53f`).
- Verify first:
  ```
  cd /Users/jwm/Projects/donner/.claude/worktrees/fixdrag
  git rev-parse --short HEAD   # expect 774b6ff1
  git branch --show-current    # expect v08/fix-drag-compose
  git status --porcelain       # expect clean (ignore stray .ec* tooling files; rm them)
  ```
  If HEAD differs: `git reset --hard 774b6ff1`. Work **only** in this worktree — do not touch
  the main repo or sibling `.claude/worktrees/*` (parallel work edits `PenTool.cc`,
  `EditorShell.cc`, `MenuBarPresenter.cc` there).
- **Commit early and often** on `v08/fix-drag-compose`; do **not** push. Re-check
  `git rev-parse HEAD` after each commit (a rogue background shell caused branch resets earlier;
  if HEAD jumps, reset to your last good commit).
- **Every** bazel command must pass `--disk_cache=/Users/jwm/.cache/donner-bazel-shared`.
  Don't run concurrent bazel servers against this worktree.

The two commits already on the branch above base `c0e8c53f`:
- `2087fbcb` Fix translation-only drag compose-offset (preexisting compositor bug)  — RC#1
- `774b6ff1` Carry forward static-segment generation across unchanged hint-kind re-promote — RC#2

All edits so far are in `donner/svg/compositor/CompositorController.cc`.

---

## What's already fixed (keep it)

**RC#1 — immediate-layer re-raster clobbered the translation compose offset.**
A cheap promoted layer becomes an *immediate* (direct-render) layer. The rasterize-dirty-layers
loop in `CompositorController::renderFrameImpl` (~line 2080) re-rasterized immediate layers on
**every** frame because `layer.isImmediate()` bypassed the dirty check. On a pure-translation
drag the fast path had already written the drag delta into `canvasFromBitmap_` for bitmap reuse,
but the unconditional re-raster called `setBitmap`, which **resets `canvasFromBitmap_` to
identity** and bumps the texture generation. Net symptom: `layerComposeOffset()` returned `(0,0)`
and generation/rasterizeCount climbed 1→6 on a pure translation.
Fix: skip the immediate re-raster when the layer is clean, payload-backed, has a stamp, and its
`canvasFromBitmap` is a pure translation (`immediateTranslationOnlyReuse`).
Also fixed `hasSplitStaticLayers()` (~line 1060): it early-returned `true` on the first
`ActiveDrag` hint without counting total live hints, so ≥2 promoted layers looked like a single
split target — now counts live (non-pending-demote) hints and reports split only when exactly one.
**Result: `//donner/svg/compositor:compositor_tests` is 50/50 GREEN** (was red at base).

**RC#2 — static-segment generation churn (partially addressed, VERIFY).**
`rasterizeDirtyStaticSegments` (~line 2694: `staticSegmentGeneration_[i] = nextTileGeneration_++`)
and `resyncSegmentsToLayerSet` (~line 2790) minted fresh generations for reused/unchanged segment
slots when a hint-kind change (Selection→ActiveDrag) re-promote shifted segment boundaries.
Commit `774b6ff1` carries the generation forward across an unchanged re-promote.
**VERIFY** it fully fixed `compositor_golden_tests.SelectionToActiveDragDoesNotAdvanceUnchangedTileGenerations`
(was tile 5 generation 16 vs expected 12); finish it if still partial.

---

## What's still RED (drive all to green)

Run targets individually first to reproduce, e.g.:
```
bazel test --disk_cache=/Users/jwm/.cache/donner-bazel-shared \
  //donner/svg/compositor:compositor_golden_tests \
  --test_arg=--gtest_filter='CompositorGoldenTest.SelectionToActiveDragDoesNotAdvanceUnchangedTileGenerations'
```

1. **`//donner/svg/compositor:compositor_golden_tests`**
   - `SplitBitmapsStableAcrossTranslationOnlyDragFrames`
   - `SelectionToActiveDragDoesNotAdvanceUnchangedTileGenerations` (RC#2 — confirm fixed)
   - `NonPromotedMutationInvalidatesOnlyContainingSegment`
   - `TightBoundedSegmentsPixelIdentityOnRealSplashWithDrag` — **RC#4**: ~4190px mismatch.
     Tight-bounds segment compose recrops/shifts splash pixels under drag. Look at
     `rasterizeDirtyStaticSegments` / `composeLayers` applying the compose offset **without
     recropping** (the segment's source rect must move with the offset, not get re-cropped at the
     old bounds).
   - `SplashLetterThenCloudOrbSelection`
   - `SplashCloudsDragMatchesReference`

2. **`//donner/editor/tests:async_renderer_tests`** — **RC#3 + RC#5**
   - `ActiveDragStartDoesNotAdvanceUnchangedTileGenerations`,
     `SteadyActiveDragTargetReusesPublishedTextureMetadataOnly` — **RC#3**: the editor GL-upload
     path in `donner/editor/AsyncRenderer*` / `donner/editor/CompositedPresentation*` re-uploads
     the drag-target texture **every frame even when the tile generation is unchanged**. It should
     publish **metadata only** (transform/offset) when the generation hasn't changed, not re-upload
     pixels.
   - **RC#5 — SEGFAULT**: this target **segfaults mid-suite, and it segfaults at clean base
     `c0e8c53f` too** (verified by swapping the base `CompositorController.cc` back in). So the
     segfault is independent of RC#1/#2 and must be root-caused on its own — run the target, get the
     crash, backtrace it (lldb, or a `-c dbg` build), fix it. This is in scope (no preexisting-issue
     exemption).

3. **`//donner/editor/tests:editor_layer_stress_tests`** — writeback pixel diffs (3527–10401 px);
   likely downstream of RC#3/#4.

4. **`//donner/editor/tests:rnr_replay_tests`** —
   `FilterDisappearRepro3MatchesGoldenAfterSecondMouseUp`,
   `DeleteElementDoesNotResetPreviouslyMovedShapes`.

5. **`//donner/editor/tests:gl_rnr_replay_tests`** —
   `GeodeDragZoomRerasterizesDonnerDOverlayEveryPresentedFrame`.

6. **`//tools/mcp-servers/editor-control:editor_control_session_tests`** —
   `SplashOThenRDragKeepsStableSplitLayerPaintOrder` (emits `unknown:immediate:… translation_doc
   {0,0}` tiles instead of `layer:…` tiles carrying the drag delta).

Likely remaining root causes, by area: **RC#3** GL re-upload metadata-only (AsyncRenderer /
CompositedPresentation), **RC#4** tight-bounds segment compose recrop (CompositorController
segment compose), **RC#5** the async_renderer segfault. RC#2 may already be done. Several editor/
golden failures will likely clear once RC#3/#4 land.

---

## Method (strict — CLAUDE.md)

1. Reproduce each failure first (run the target, capture the assertion or the crash backtrace).
2. Root-cause it in **production** code; the tests encode the correct contract.
3. Confirm red→green for that target; commit.
4. **Never** edit/disable/skip/delete a test to make it pass.
5. **"Anti-aliasing" is a banned root-cause for any pixel diff** — the pixelmatch harness already
   excludes AA, so any flagged diff is a real bug (wrong transform / offset / crop / generation /
   premultiply). A 4190px mismatch is definitely not AA.
6. Remove any debug `fprintf`/scratch before committing. Run
   `/opt/homebrew/opt/llvm@18/bin/clang-format -i` on edited files. Respect the **destFromSource**
   transform-naming convention (e.g. `bitmapEntityFromEntity`, `canvasFromBitmap`).

## Final gate

```
bazel test --disk_cache=/Users/jwm/.cache/donner-bazel-shared --flaky_test_attempts=2 //...
```
→ 0 failures. If you can't reach full green, commit all real progress and report exactly which
targets remain red with assertion/backtrace evidence and a next-step hypothesis — do not fake
green or relabel anything "preexisting/out of scope".

## When done

Report the final `v08/fix-drag-compose` HEAD SHA, each target turned green with its root cause,
the segfault root cause, and the full `bazel test //...` summary line. Then this branch merges
into `v0_8_drive` (squash-and-merge per CLAUDE.md when it eventually lands on `main`).
