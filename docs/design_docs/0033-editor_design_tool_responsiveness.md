# Design: Editor Design-Tool-Grade Responsiveness

**Status:** Implementing (M1, M2, M5, M7, M8, M9 landed; M3/M4/M6/M10 open)
**Author:** Claude Opus 4.7 (1M context)
**Created:** 2026-05-12
**Last updated:** 2026-05-13

## Summary

Donner's editor today goes catastrophically slow at moderate-to-high zoom on
documents with promoted filter / mask / clip-path layers — multi-second per-
frame freezes during pinch-zoom, multi-second "click delay before drag" on
first interaction at a new zoom level, and resolution that visibly drops and
fails to recover as the user works. The proximate causes are well understood
after a tier-3 investigation (layer rasterize is canvas-sized; every canvas
size change triggers `invalidateRenderTree` → all layers dirty → seconds of
sequential rasterization on the critical path; the editor blocks new clicks
behind that work via `isBusy()`). The fixes need a coherent architectural
direction rather than another round of point fixes.

This doc proposes the architectural shift from "rasterize the full canvas
synchronously when state changes" to a Figma / browser-class model: rasterize
layers at their *intrinsic* size, scale them on display via GPU during
gesture, refine asynchronously, and never block user input on a high-res
render. It also calls out a diagnostic panel that exposes the live compositor
layer state to the operator so future regressions can be triaged without
re-running the long-form instrumentation cycle that this round needed.

## Goals

1. **Click-to-first-pixel < 100 ms at any zoom on `donner_splash.svg`**, with
   the first pixel being a "good enough" preview (current cached bitmap
   stretched + path overlay sharp) and the high-resolution refinement
   arriving asynchronously without blocking subsequent interaction.
2. **Drag frame budget < 33 ms (30 fps) at the editor's max zoom**, sustained
   across `donner_splash.svg`'s 7-layer filter cascade. No multi-second
   freezes mid-drag; no "pile-up" symptom where the editor stays stuck after
   an interaction.
3. **Pinch-zoom is GPU-stretch during the gesture**, with a single high-res
   re-rasterize after the gesture settles. The user never sees a multi-
   second freeze tied to the gesture itself.
4. **Resolution monotonically improves**, never regresses. Once a layer has
   been rasterized at scale S, the editor never displays it stretched from a
   lower-scale bitmap unless it's *intentionally* invalidating that cache
   (canvas size change, document structure change). The "low-res persists
   until I zoom again" symptom should be impossible by construction.
5. **Selection chrome (path outline + AABB) renders at full resolution
   immediately** — the chrome is geometrically cheap (single-color overlay
   bitmap) and must never be deferred behind layer rasterization. A user who
   clicks an element sees the selection chrome appear in the next frame even
   if the underlying canvas is still warming up.
6. **Incremental refinement is preemptive**: when a higher-resolution
   rasterize lands, the editor swaps it in *immediately* — not on the next
   mouse event, not on the next dirty-flag bump. The user sees the upgrade
   the moment it's ready.
7. **Diagnostic panel exposes live compositor state** — layer count,
   per-layer thumbnails, bitmap canvas size, rasterize wall-clock,
   cache-hit/miss counters — so the operator can answer "why is this slow
   *right now*" without instrumenting the source each time.

## Non-Goals

- **Vector-level animation / morphing.** The doc covers static-document
  manipulation. SMIL / CSS animations have their own compositor invariants
  (see `0025-composited_rendering.md`).
- **Multi-document editing.** Single-document workflow; the layer panel
  and caches are scoped to the active document.
- **Reverting `<g filter>` auto-promotion**. The compositor's mandatory
  promotion of filter groups is a hard invariant of how the renderer
  handles filters; the responsiveness work has to live with the promotion,
  not around it.
- **General-purpose tiling.** Figma-style tile-cache is a feature for
  multi-MB documents with non-overlapping regions; Donner's typical
  document fits comfortably in a single canvas. We'll revisit if
  document-size growth pressures the layer-bitmap memory budget.
- **Sub-1-frame drag latency on the tiny-skia backend.** With tiny-skia
  (CPU) as today's default, we target 33 ms (30 fps) sustained for
  drag at the editor's max zoom. 16 ms / 60 fps is the target *once
  Geode lands as the default*; the architecture in this doc is designed
  so the same compose / cache / preempt machinery accelerates with
  Geode (GPU-tessellated path rasterization via the Slug algorithm)
  without a second rewrite.

## Status (2026-05-13)

| Milestone | State | Commit |
|-----------|-------|--------|
| M1 — Diagnostic layer panel | ✅ Landed (expanded scope: paint-order tile table, state header, viewport diagnostics) | `e38d9a94` + iterations folded into `ab802105` |
| M2 — Intrinsic-size rasterization | ✅ Landed (M2A+M2B squashed; zoom-fix follow-up) | `e5a08619`, `4f0f5bf6` |
| M3 — Cache band invariant under canvas resize | ⏳ Open | — |
| M4 — Async re-rasterization with cancellation | ⏳ Open | — |
| M5 — Preemptive swap-in | ✅ Landed | `793f96eb` |
| M6 — Pinch-zoom GPU stretch | ⚠️ Partial (canvas-commit throttle only) | `0d1cdc56` |
| M7 — Selection chrome priority lane | ✅ Snapshot infra landed; editor callsites not flipped yet | `965f0a02` |
| M8 — Click→drag handoff doesn't wait for raster | ✅ Cache-backed re-drag fast path bypasses `!isBusy()`; full hitTest path still gated | (M8 re-attempt) |
| M9 — Layer-set hysteresis | ✅ `demoteEntity` queues for `kDemotionHysteresisFrames` (30, ~0.5s @ 60Hz); re-promote inside the window cancels the demote without segment churn | (this commit) |
| M10 — Operator perf validation | ⏳ Open | — |

### Post-implementation stabilization (debugging arc, folded into `ab802105`)

The M1/M2 landing surfaced a cluster of pre-existing bugs that the
old `[CompositorSlowFrame]` log had been masking. All fixed in the
omnibus stability commit:

- **Prewarm dispatch loop:** entities the compositor refused to
  promote (filter ancestors, memory cap) were re-queued every frame.
  Closed via `notePrewarmAttempted` tracking.
- **Drag dispatch loop:** the parallel `needsExperimentalLayerCapture`
  path had the same shape. Closed via a `sameAsLastFailedDispatch`
  guard.
- **`cachedCanvasSize` mismatch:** `noteCachedTextures` was called
  with the promoted bitmap's pixel dimensions instead of
  `SVGDocument::canvasSize()`. After M2's intrinsic-size promotion
  these aren't equivalent — the cache freshness check rejected
  every frame and triggered a re-dispatch loop. Fixed by passing the
  live document canvas size.
- **Memory budget:** `kMaxCompositorMemoryBytes` was 256 MiB.
  Retina-scale splash hit the cap at ~252 MiB and refused promotion
  with `PromoteRefusalReason::MemoryLimit` — visible from the panel.
  Bumped to 1 GiB.
- **Canvas-commit stall after `ReplaceDocument`:** the throttle
  compared the pending desired canvas against a stale
  `lastSetCanvasSize_` cache that was never invalidated when
  `ReplaceDocument` installed a fresh registry (with `canvasSize=
  nullopt`). Fix: compare against the live
  `app.document().document().canvasSize()` readback with a 1-pixel
  aspect-rounding tolerance.

## Next Steps

- **M3 (cache band) is the next perf-impacting milestone.** Layer
  bitmaps now survive a canvas resize (M2), but the editor still
  re-rasterizes whenever the viewport scale moves at all. The
  scale-band tolerance is what turns "drag at a new zoom level
  re-rasterizes everything" into "drag at a new zoom level
  GL-stretches the cached bitmap until the worker delivers a
  refined one."
- **M8 selection-change decoupling.** The M8 fast path covers
  re-drag (click inside currently-selected element). Selection
  change clicks still gate on `!isBusy()`. Routing the new selection
  through M7's `SelectionChromeSnapshot` end-to-end would let the
  selection switch also bypass `!isBusy()`. Sequenced after M3 so
  the cache-band tolerance can serve the post-selection-change
  preview without a forced re-rasterize.
- **M6 completion.** Once M3 lands, the pinch-zoom GPU-stretch
  model becomes a thin shim on top of the cache band — the cached
  bitmap is already authoritative; the pane's display transform
  just absorbs the gesture.
- **M9 follow-on memory cap.** With sustained "select different
  element each time" churn the pending-demotion queue could in
  principle grow unbounded. The `kMaxCompositorLayers` ceiling on
  `activeHints_` already caps it indirectly (a pending-demote
  layer counts toward the layer slot budget, so a 33rd hint would
  be refused), but an explicit LRU eviction on the
  `pendingDemotions_` map would be cleaner. Trivial to add when
  needed.
- **M10 (perf validation).** No per-milestone wall-clock budget
  tests are wired up yet; budgets live only as comments in the
  table below.

## Implementation Plan

- [x] **Milestone 1: Diagnostic layer panel.** *Landed in `e38d9a94`;
  follow-on iterations folded into the omnibus `ab802105`.*
  Right-side ImGui panel with one row per active compositor layer; columns
  are (id, entity name/id, kind/source, bitmap canvas size, rasterize
  wall-clock from last frame, cache-hit count, fast-path-engaged
  count, last-rasterize-trigger reason, thumbnail). Read-only — observation
  surface, not a control. Updates each frame the renderer isn't busy
  (the SidebarPresenter's existing snapshot pattern).

  *As shipped, the panel goes beyond the original scope:*
  - **Unified paint-order tile table** — one row per composite tile
    (background, foreground, segments, layers) with thumbnails, so the
    panel reflects exactly what the renderer blits to make the final
    frame. Replaces the originally-planned per-layer-only view.
  - **State header** surfaces `activeHints`, `layerCount`, `splitPath`,
    canvas size, and the last `PromoteRefusalReason` (`InvalidEntity`,
    `CompositingBreakingAncestor`, `LayerLimit`, `MemoryLimit`,
    `DescendantPromoted`). The `MemoryLimit` repro on splash at Retina
    was diagnosed entirely from this header — no source-level
    instrumentation needed.
  - **Viewport diagnostics** — `zoom`, `DPR`, and a three-way canvas
    state (viewport-desired vs. document-committed vs. compositor-
    rasterized). Red on commit stall vs. desired; orange on compositor-
    not-yet-re-rasterized. Surfaced the `ReplaceDocument` canvas-
    commit stall bug fixed in the omnibus.
  - APIs: `CompositorController::snapshotState()`,
    `snapshotCompositeTiles()`, `snapshotLayerInspectorRows()` — all
    self-contained value types, safe to read off the worker thread.

- [x] **Milestone 2: Intrinsic-size layer rasterization.** *M2A landed
  in `e5a08619`, M2B in the same commit (squashed); follow-up zoom-fix
  in `4f0f5bf6`.*
  Each promoted layer rasterizes into a bitmap sized to the *layer's
  worldBounds + filter padding*, not the full canvas. Compose places the
  bitmap at the layer's canvas offset. Removes the "all layer bitmaps are
  canvas-sized, every canvas-size change invalidates everything" failure
  mode. The bitmap stores its source-scale and its world-to-bitmap
  transform so the fast-path delta math survives a canvas resize.

  *As-shipped notes:*
  - `computeEntityRangeBounds` + a 2px AA halo defines the intrinsic
    bitmap; `CompositorController::canvasOffset_` carries the layer's
    canvas-space offset through to the editor blit.
  - The M2B blit initially pinned promoted-layer bitmaps at their
    on-disk pixel size (`bitmapPx / DPR`), so zoom-in stretched the
    chrome but not the promoted bitmap — content drifted off its
    path. Fixed in `4f0f5bf6` by adding a `promotedBitmapDimsDoc`
    field that scales the blit destination by the live
    `documentFromCanvas`.
  - Related regression discovered post-M1: when `SVGDocument::
    setCanvasSize` calls `invalidateRenderTree`, every
    `RenderingInstanceComponent` is wiped. If `mandatoryDetector_.
    reconcile` ran before `prepareDocumentForRendering` rebuilt the
    RICs, the detector saw zero candidates and dropped the filter
    layer. Reordering fix lives in the M1 commit's diff and is
    pinned by `CompositorController_tests.cc::
    MandatoryFilterLayerSurvivesCanvasResize`.

- [ ] **Milestone 3: Cache key invariant under canvas resize.**
  A layer's bitmap remains valid across a canvas-size change as long as the
  rasterized scale is "good enough" for the new viewport. Define a per-
  layer `scaleRatio = bitmap_pixels / current_viewport_pixels`; if
  `scaleRatio ∈ [0.5, 2.0]` use the cached bitmap with a compose-time
  scale; outside the band, schedule a re-rasterize. The threshold is
  asymmetric vs Figma's stricter ±1 stop because most user interactions
  fall within one zoom step.

- [ ] **Milestone 4: Async re-rasterization with cancellation.**
  Re-rasterize requests run on the AsyncRenderer's worker. While a stale-
  scale bitmap is being refined, the editor continues displaying the
  stretched cached bitmap. New requests *cancel* in-flight requests that
  haven't started (FIFO drained on `pendingRequest_` overwrite, like
  drag-coalesce). Critical: never block UI thread on a re-rasterize, and
  never queue more than one in-flight request per layer.

- [x] **Milestone 5: Preemptive swap-in.** *Landed in `793f96eb`.*
  When the AsyncRenderer's worker delivers a refined-resolution bitmap,
  the editor's main loop swaps it in on the *next ImGui frame*, not on the
  next mouse event. The current setup waits for `pollResult()` to be
  called from the user's event-handling path, which can lag by hundreds of
  ms during idle. Add an explicit `wake()` from the worker that triggers
  ImGui to redraw via `glfwPostEmptyEvent`.

  *As shipped:* `AsyncRenderer::setWakeCallback` is wired to
  `Window::wakeEventLoop` in `EditorShell::EditorShell` (which calls
  `glfwPostEmptyEvent` under the hood); the worker fires it the moment
  a refined-resolution result is ready. Pinned by
  `AsyncRenderer_tests.cc` (the test asserts the callback is invoked
  exactly once per ready result, regression-guarding the wake plumbing).

- [ ] **Milestone 6: Pinch-zoom GPU stretch.** *Partial — canvas-commit
  throttle landed in `0d1cdc56` (`kCanvasSizeCommitDelay = 120 ms` in
  `RenderCoordinator`), but the full "no commit during gesture +
  GPU-stretch the cached bitmap" model is not yet implemented.*
  During an active pinch gesture, no `setCanvasSize` commit. The pane's
  display-transform absorbs the pinch zoom via GL stretch on the cached
  bitmap. After gesture-end + 200 ms idle, commit the new canvas size and
  schedule the async re-rasterize. Same model browsers use for trackpad
  pinch.

  *Status:* the throttle is sufficient to prevent the worst-case
  setCanvasSize-per-event storm, but the editor still re-rasterizes
  mid-gesture once the throttle window elapses. The compositor-side
  `ReplaceDocument` canvas-commit stall (fixed by comparing against
  the live `SVGDocument::canvasSize()` readback in
  `RenderCoordinator.cc`, with a 1-pixel aspect-rounding tolerance)
  was a prerequisite — without that, the throttle would mask the
  stall and re-rasterizes would silently use the wrong canvas size.

- [x] **Milestone 7: Selection chrome priority lane.** *Snapshot
  infrastructure landed in `965f0a02`. Editor callsites still gate on
  `!isBusy()`; flipping them to the snapshot path is what M8 needs to
  unblock.*
  The overlay renderer's chrome rasterize moves to a high-priority slot
  that runs before any layer rasterize on the AsyncRenderer worker (or
  on the UI thread if cheap enough — the chrome is a single offscreen
  bitmap with one path outline + AABB). User sees the selection appear in
  the frame after the click; high-res content can lag.

  *As shipped:*
  - `OverlayRenderer::captureChromeSnapshot(selection, marquee,
    canvasFromDoc)` reads the registry once and packs everything the
    draw phase needs into a self-contained `SelectionChromeSnapshot`
    value (per-element `(spline, canvasFromElement)`, AABBs in doc
    space, optional marquee, pre-computed stroke widths). Holds no
    registry pointers.
  - `OverlayRenderer::drawChromeFromSnapshot(renderer, snapshot)`
    reads only the snapshot — race-free with respect to the worker.
  - The existing `drawChromeWithTransform` API is preserved and now
    routes through `captureChromeSnapshot` + `drawChromeFromSnapshot`
    internally, so the snapshot path is the single source of truth.
  - Pinned by `SnapshotProducesByteIdenticalPixelsAsLivePath` (API
    equivalence) and `SnapshotSurvivesDocumentMutationBetweenCapture
    AndDraw` (race-safety).

- [x] **Milestone 8: Click→drag handoff doesn't wait for raster.**
  *Cache-backed re-drag fast path landed; the first M8 attempt
  (`47901a55`) was reverted because it called the live
  `SnapshotSelectionWorldBounds` mid-render and raced the worker's
  `prepareDocumentForRendering`. The re-attempt routes the bounds
  read through `SelectionBoundsCache::displayedBoundsDoc` —
  populated on idle frames — so the call doesn't touch any
  registry component the worker is mid-mutating.*

  *As shipped:*
  - `SelectTool::tryStartRedragOnSelected` accepts a caller-supplied
    `std::span<const Box2d> selectionBoundsDoc`. EditorShell passes
    the bounds-cache snapshot; `onMouseDown` (slow path, already
    inside `!isBusy()`) passes a freshly-computed live snapshot.
  - `EditorShell` calls `tryStartRedragOnSelected` BEFORE checking
    `!isBusy()`. If the click falls inside the cached bbox of the
    currently-selected single element, the drag starts immediately
    — no waiting for the worker. The follow-up registry-reading
    work (bounds cache refresh, overlay rasterize, request render)
    is deferred to the next idle frame via
    `pendingClickFollowupAfterIdle_`, so the chrome catches up
    within 1–2 frames of the click without blocking the click
    acknowledgement.
  - Slow path (selection change, marquee, shift-click, multi-select,
    stale cache) still gates on `!isBusy()` and goes through the
    full `onMouseDown` flow. Selection change is rare relative to
    re-drag during sustained editing, so the slow-path latency is
    acceptable; further decoupling (route the new selection through
    M7's chrome-snapshot end-to-end) is future work.
  - Pinned by `SelectTool_tests.cc`:
    - `TryRedragOnSelectedStartsDragWhenClickIsInsideSelectedBounds`
    - `TryRedragOnSelectedReturnsFalseOnShiftClick`
    - `TryRedragOnSelectedReturnsFalseWhenNothingSelected`
    - `TryRedragOnSelectedReturnsFalseWhenClickIsOutsideSelectedBounds`
    - `TryRedragOnSelectedReturnsFalseOnEmptyCachedBounds`
    - `TryRedragOnSelectedHitsTransparentInteriorOfFiltergroup`

- [x] **Milestone 9: Layer-set hysteresis.** *Landed.*
  Adding/removing a promoted layer (mandatory filter detector running
  on a freshly-mutated document, drag-target promotion, etc.) currently
  resets `resyncSegmentsToLayerSet`'s caches. Add hysteresis: promote
  immediately; demote only after the entity has been below the threshold
  for several frames. Prevents the "click-deselect-click" trash-and-
  rebuild loop.

  *As shipped:*
  - `CompositorController::demoteEntity` adds the entity to a
    `pendingDemotions_` map keyed on entity → frame counter
    (`kDemotionHysteresisFrames = 30`, ~0.5s at 60Hz). The hint
    stays in `activeHints_`; the layer stays in `layers_`; segments
    stay split. No `resolver_.resolve` / `reconcileLayers` /
    `resyncSegmentsToLayerSet` work fires synchronously.
  - `promoteEntity` for the same entity inside the window erases
    the pending-demote entry and falls through to the existing
    kind-refresh path. The cached bitmap and segment split are
    reused — exactly zero compositor work beyond an
    `unordered_map::erase` and an interaction-kind write.
  - `renderFrame` calls `processPendingDemotions` once at the top
    to age each counter by one. Entries that hit zero are erased
    from `activeHints_`; the deferred `resolver_.resolve` +
    `reconcileLayers` runs in a single batched pass for any number
    of expirations in the same frame.
  - `flushPendingDemotionsForTesting()` is exposed for unit tests
    that pre-date M9 and expect the "promote → demote → assert
    layer gone" pattern. Production code never calls it; it just
    drops the per-entry counters to zero and runs
    `processPendingDemotions` immediately.
  - `resetAllLayers` clears `pendingDemotions_`;
    `remapAfterStructuralReplace` remaps the map keys alongside
    `activeHints_` so the queue survives `ReplaceDocument` /
    structural-equivalent reparses.
  - Pinned by `CompositorController_tests.cc::M9*`:
    - `M9DemoteIsLazyWithinHysteresisWindow` — demote leaves the
      layer in place for the duration of the window.
    - `M9RepromoteSameEntityCancelsPendingDemote` — same-entity
      re-promote reuses the cached bitmap (generation does not
      bump).
    - `M9DemoteFiresAfterHysteresisExpires` — after
      `kDemotionHysteresisFrames + 1` renderFrame calls the
      deferred resolver pass runs and the layer is gone.
    - `M9FlushPendingDemotionsForTesting...` — the test-helper
      bypass works.

- [ ] **Milestone 10: Operator perf validation.**
  Each prior milestone has a per-milestone perf gate in
  `donner_perf_cc_test`-style targets (correctness counters on the PR
  gate; wall-clock budgets on the nightly perf lane). The whole stack
  meets the goals stated above on the `donner_splash.svg` corpus with
  the standard editor input recordings (`filter_elm_disappear-*.rnr`)
  augmented by new zoom-then-drag recordings.

## Background

The tier 3 editor port from sandbox surfaced three intertwined regressions
during operator testing on 2026-05-12:

1. After pinch-zooming, the first click+drag freezes for multiple seconds.
2. After multiple drags at high zoom, resolution decreases and stays
   degraded until the user zooms again.
3. The compositor occasionally "wedges" — drag inputs stop registering
   until the in-flight render queue drains.

All three trace to the same architectural fact: every promoted compositor
layer rasterizes at **full canvas size**, every canvas resize forces
`invalidateRenderTree` which marks every layer dirty, and every render
runs on the UI-blocking critical path through `AsyncRenderer::isBusy()`.
At the editor's max zoom on the splash (canvas ~8192×4710, 7 promoted
filter-group layers), a full re-rasterize is ~12 s. The editor blocks
clicks and drags for the duration.

Patch-level mitigations (debouncing `setCanvasSize`, skipping main compose
while a selection is active, refreshing interaction kind in place to keep
the split-bitmap path engaged) recovered some of the per-frame budget but
hit a ceiling — they can't eliminate the work, only reorder it. The
operator's verdict on the latest iteration: **"Still lots of issues … some
pileup happening maybe."**

Prior art the doc draws from:

- **Figma** rasterizes vector paths to triangle meshes on the GPU, ties
  layer caches to *intrinsic* size + transform-on-compose, and tiles
  documents over a certain memory threshold. This is the model Donner
  is moving toward: Geode (WebGPU + the Slug algorithm) is the target
  renderer, and the intrinsic-size cache + transform-on-compose
  structure proposed here is the *backend-agnostic* layer that lets the
  same compositor work for both tiny-skia today and Geode once it
  ships. Until Geode is the default, the cache absorbs the gap by
  keeping previously-rasterized bitmaps reusable across zoom changes
  rather than re-tessellating per frame.
- **Sketch** caches each layer at a "natural" bitmap size and refines
  asynchronously when the user zooms past the cache's quality threshold;
  meanwhile the cached bitmap displays stretched. This is the model
  Donner's compositor is closest to today — it has the layer bitmaps,
  it just doesn't refine asynchronously.
- **Safari / Chrome trackpad pinch-zoom** is purely a GL transform during
  the gesture; the renderer is only re-invoked after the user lifts. This
  is the right model for Donner's pinch path.
- **VS Code editor** runs heavy work on a worker, never blocks the UI
  thread on it, and preempts in-flight work when new state arrives. The
  AsyncRenderer is structurally similar; what's missing is the preemption
  + the "don't wait for the next event to swap result in" plumbing.
- **Procreate / Photoshop** use coarse-to-fine mipmapped rasters at multiple
  zoom levels and pick the closest one per frame; the levels are
  pre-rendered during idle and stored against a quota. Out of scope for
  the first cut — we can revisit if the in-band quality threshold proves
  insufficient.

## Proposed Architecture

```
                 ┌─────────────────────────────────────────────────────┐
                 │                  EditorShell (UI thread)            │
                 │                                                     │
   click/drag ──►│  SelectTool (sync, ms): selection + drag state      │
                 │                              │                      │
                 │                              ▼                      │
                 │  RenderCoordinator:    request to AsyncRenderer     │
                 │  - drag-coalesce       (canvas size frozen during   │
                 │  - canvas debounce      pinch; only commits at gesture
                 │                         settle)                     │
                 │                              │                      │
                 │  CompositedPreviewCache◄─────┤      ▼               │
                 │  - bg/promoted/fg            │  AsyncRenderer       │
                 │    at intrinsic scales       │  (worker thread)     │
                 │  - scale-band check          │                      │
                 │      hit  ─► reuse + stretch │  CompositorController│
                 │      miss ─► schedule refine │  - layer bitmaps at  │
                 │                              │    layer worldBounds │
                 │                              │  - per-layer scale   │
                 │                              │  - refine queue      │
                 │  Display:                    │     (1 slot, FIFO    │
                 │  - bg blit @ display scale   │      preemption)     │
                 │  - promoted blit @ delta     │  - chrome rasterize  │
                 │  - fg blit @ display scale   │     priority lane    │
                 │  - chrome overlay @ full res │                      │
                 │                                                     │
                 │  (Sidebar) LayerPanel ◄─── live snapshot stream     │
                 └─────────────────────────────────────────────────────┘
```

### Key shifts from today

1. **Layer bitmaps stop being canvas-sized.** Each layer's bitmap is sized
   to `layer.worldBounds() + filterPadding`. Compose-time placement uses
   the layer's *canvas-space offset*, not the bitmap's pixel origin.
   Removes the "canvas size change = all bitmaps invalid" failure mode.

2. **Compose-time scale absorbs zoom changes within a band.** A bitmap
   rasterized at scale S can be displayed at scale `S × k` where
   `k ∈ [0.5, 2.0]` using the existing `canvasFromBitmap` transform. The
   compositor's fast path already supports non-translation deltas in
   `canvasFromBitmap`; we just need to widen the fast-path eligibility
   check from "translation only" to "translation OR uniform-scale within
   tolerance." Outside the band → async re-rasterize.

3. **The AsyncRenderer's request slot is preemptive.** A new request
   overwrites `pendingRequest_` (already true today) AND cancels the
   in-flight rasterize if it hasn't committed pixels yet (new). Required
   so a pinch-then-drag doesn't sit behind a stale post-pinch rasterize.

4. **Worker explicitly wakes the UI thread on result-ready.** Today the
   UI thread polls; if the user isn't moving the mouse, there's no
   ImGui frame, the result sits until the next event. Add
   `glfwPostEmptyEvent` from the worker on result-ready so a finished
   rasterize is visible within one frame.

5. **`onMouseDown` doesn't wait for the renderer.** Today the editor
   defers selection/drag-init behind `!asyncRenderer.isBusy()`. Move
   `SelectTool::onMouseDown` and selection chrome to the UI thread
   (cheap, no registry mutation), and let the async render catch up
   behind them.

6. **Pinch zoom doesn't commit canvas size during the gesture.** During
   active pinch, the pane's screen-side viewport transform absorbs the
   zoom via GL stretch on the cached bitmap. Settle for 200 ms idle →
   commit + schedule re-rasterize. No mid-gesture re-rasterizes.

7. **Diagnostic surface.** A right-side panel observable in any session
   (no flag) exposes the live compositor state — paint-order composite
   tiles with thumbnails, raster wall-clock, state header (active
   hints, layer count, split path, last `PromoteRefusalReason`),
   viewport diagnostics (zoom, DPR, three-way canvas state) — as an
   always-on overlay. The earlier `[CompositorSlowFrame]` stderr
   instrumentation has been removed; the panel surfaces the same data
   live (per `ab802105`).

### Strategies prod tools use to feel responsive (recommended additions)

These are the high-leverage "what makes Figma feel instant" tricks. The
plan above covers the must-haves; the items below are recommended
follow-ups once the core ships:

- **Coarse-to-fine mipmaps.** Maintain a low-detail "wireframe" raster
  of each layer (e.g. at 1/4 resolution). Display the wireframe
  *immediately* in the first frame after a layer is invalidated; the
  high-res rasterize lands within ~100 ms and swaps in. The user sees
  the right shape and color instantly; sharpness arrives over the next
  frame or two.
- **Predictive prewarming.** When the user pauses pinch-zoom for >300 ms,
  start rasterizing the *next-likely* zoom level (one stop up and down)
  in the background. Eats the next pinch-step's cost during idle.
- **Layer paint-order is dirty-stable.** A layer's *raster identity* is
  keyed on `(entityRange, scale)`, not paint-order index. The bitmap
  cache survives layer-set reshuffles (e.g. a new promotion shifting
  indices) as long as the entity range hasn't changed. Mostly already
  true today; the audit catches the remaining slot-keyed assumptions.
- **Per-layer "this is hot" vs "this is static" tier.** A layer that's
  been the drag target in the last 5 s is "hot" — always at full
  resolution, evicted last under memory pressure. The rest are "static"
  — evicted first, can drop to 1/2 resolution under pressure. Trivial
  policy, large impact on the LRU cache that the next milestone
  introduces.
- **First-pixel synthesis from the bg/promoted/fg cache.** The
  experimentalDragPresentation path already keeps three bitmaps. On a
  fresh click before any new render lands, blit those three with the
  selection chrome on top. The user sees a complete-looking frame
  immediately; only the *content* of the layer being interacted with is
  briefly stale.

## Performance

Per-milestone targets (measured on the `donner_splash.svg` corpus on the
operator's M-series Mac with Retina DPR=2):

| Milestone | Metric | Today | Target after milestone |
|-----------|--------|-------|------------------------|
| 1 (panel) | n/a — diagnostic-only | n/a | n/a |
| 2 (intrinsic size) | Single-layer rasterize wall-clock at zoom 9× | ~1500 ms | ~80 ms (1/(zoom²) reduction) |
| 3 (cache band) | Layer-bitmap reuse rate during a zoom-in to 5× | 0 % | ≥ 80 % |
| 4 (async + preempt) | UI thread blocked on raster during drag-start at zoom 5× | up to 12 s | ≤ 16 ms (one frame of buffer wakeup) |
| 5 (preempt swap) | Time from raster-complete to display-on-screen | up to ~250 ms (next event) | ≤ 16 ms (next frame) |
| 6 (pinch GPU stretch) | Worst-frame wall-clock during continuous pinch | ~1500 ms | ≤ 16 ms (GL stretch only) |
| 7 (chrome priority) | Click-to-selection-visible | up to 12 s | ≤ 33 ms |
| 8 (mousedown handoff) | Click-to-drag-begins | up to 12 s | ≤ 33 ms |
| 9 (hysteresis) | Drag-after-drag freeze | ~1500 ms | ≤ 16 ms |
| 10 (end-to-end) | Sustained drag at editor max zoom | unmeasurable (drops to ≤1 fps) | ≥ 30 fps sustained |

The measurement plan uses `donner_perf_cc_test` to gate correctness
counters (e.g. fast-path engaged, async cancel hit, mipmap-miss count)
on every PR and wall-clock budgets on the nightly `perf` lane (per
CLAUDE.md §Perf bugs).

## Testing and Validation

Each milestone ships with at least one targeted regression test:

- **M1 (panel):** snapshot test that the panel renders a row per active
  layer and that the rendered thumbnail count matches `layers_.size()`.
- **M2 (intrinsic size):** golden-image test that a layer rasterized at
  `worldBounds()` size composites byte-identical to the previous canvas-
  sized rasterization (modulo aliasing at the edges, ≤ 50 pixel diff via
  `bitmap_golden_compare`).
- **M3 (cache band):** counter assertion that a drag at zoom 5× with no
  prior cache hits 80 % bitmap reuse on the second drag.
- **M4 (async + preempt):** programmatic test that issuing two requests
  in flight cancels the first; counter increments on the cancel path.
- **M5 (preempt swap):** end-to-end test using the `.rnr` replay
  framework that a raster-complete event causes the editor to redraw on
  the next ImGui frame (asserted via a frame-count delta).
- **M6 (pinch GPU stretch):** wall-clock test that a 60-event pinch
  sequence produces ≤ 1 setCanvasSize commit and ≤ 1 layer rasterize.
- **M7 (chrome priority):** assertion that overlay rasterize wall-clock
  is ≤ 5 ms for the splash worst case (single selected path with
  selection AABB).
- **M8 (mousedown handoff):** programmatic test that `onMouseDown`
  returns without waiting on `isBusy()`.
- **M9 (hysteresis):** counter test that promote/demote churn under a
  click-deselect-click stress sequence stays bounded.
- **M10 (end-to-end):** integration perf test driving the full splash
  + filter-disappear corpus, asserting wall-clock budgets per the table
  above.

Per `AGENTS.md §Invariants Must Point At CI Targets`, each "X cannot
happen" claim in this doc maps to a named test target:

- **"Layer bitmap reuse rate across canvas resize is non-zero"** →
  `donner/svg/compositor/CompositorGolden_tests.cc::IntrinsicSizeBitmapSurvivesCanvasResize`
- **"`onMouseDown` is never blocked on `isBusy()`"** →
  `donner/editor/tests/SelectTool_tests.cc::MouseDownDoesNotWaitForAsyncRenderer`
- **"Preemptive swap delivers result within one frame"** →
  `donner/editor/tests/AsyncRenderer_tests.cc::ResultLandsOnNextImguiFrame`
- **"Pinch-zoom never commits canvas size mid-gesture"** →
  `donner/editor/tests/RenderPaneGesture_tests.cc::PinchHoldsCanvasSize`

(Each target is created in the corresponding milestone PR; gaps without
a named test are recorded as Open Questions until closed.)

## Risks

- **R1 — Intrinsic-size rasterization breaks filter regions.** Filter
  outputs can extend beyond the input bbox (gaussian blur, drop shadow,
  morphology dilate). The bitmap-size calculation must respect each
  filter primitive's expansion. Mitigation: extend the existing
  `computeBlurPadding` to a general `computeFilterPadding(graph)` that
  walks the filter graph and unions each primitive's spatial impact.
  Validated by the existing filter golden suite (every filter in
  `donner/svg/renderer/testdata/`).
- **R2 — Compose-time scaling breaks pixel-grid alignment.** Drawing a
  bitmap at non-integer scale through `drawImage` can shift pixels by
  half a unit and create visible artifacts on stroked paths. Mitigation:
  snap compose-time scale and translation to integer pixels when the
  bitmap is "close enough" to identity; only allow non-integer scale
  inside the cache-band tolerance, where the user is already in a
  transient stretched state.
- **R3 — Async cancellation racing with worker.** A worker that has
  started rasterizing but hasn't committed pixels must check a cancel
  flag at safe points; otherwise the cancellation either no-ops (slow)
  or corrupts shared state (worse). Mitigation: cancellation checks
  only between `rasterizeLayer` calls, not mid-rasterize. Each layer
  rasterize is short enough (≤ 100 ms at intrinsic size) that this is
  fine-grained enough.
- **R4 — Preemptive UI-thread wake-up loops.** A worker that wakes the
  UI thread on every result-ready can flood the event queue. Mitigation:
  the wake is `glfwPostEmptyEvent`, which is idempotent — multiple
  wakes between ImGui frames coalesce.
- **R5 — Diagnostic panel UI is not load-bearing.** The panel is a
  debugging tool, not a feature. If it stops updating because the
  renderer is wedged, that's diagnostic too. We don't add health-
  checking on top.

## Decisions

Captured during the design review with the operator (2026-05-12):

- **D1 — Scope is drag, not text editing.** The structured-text-editing
  doc covers source-pane work; this doc only addresses canvas-side
  manipulation.
- **D2 — Layer panel is right-side, read-only, ships in M1.** It's a
  diagnostic surface, not a control surface. Click-through to navigate
  tree-view is out of scope for the first cut.
- **D3 — Cache band is asymmetric: bigger toward "blurrier".** A
  bitmap displayed at 2× its rasterized scale stretches and looks soft
  (acceptable transiently). A bitmap displayed at 0.5× its rasterized
  scale downsamples and loses detail (worse). Compromise: ±1 stop in
  the "blurrier" direction, 1/2 stop in the "sharper" direction.
- **D4 — Preempt at request boundaries, not mid-rasterize.** Per R3
  above. Worst case: a stale 100 ms rasterize lands after the user has
  moved on; trivial to ignore (the editor checks request version and
  drops stale results).
- **D5 — Selection chrome runs on the UI thread for the splash worst
  case.** Profiling will confirm; the AsyncRenderer is overkill for a
  single-color overlay that's < 5 ms to rasterize.
- **D6 — Layer panel updates are gated on renderer idle.** Matches the
  existing SidebarPresenter pattern; avoids reading transient
  registry state mid-worker.

## Open Questions

- **OQ1 — What's the right intrinsic size when an entity has no
  bounding box (e.g. a `<pattern>` reference, empty `<g>`)?**
  Probably skip those entirely — they don't render. But the resolver
  may still produce a `ComputedLayerAssignmentComponent` for them.
  Need to audit `reconcileLayers` to confirm.
- **OQ2 — Should the cache band consider per-layer cost?**
  A 100×100 layer can refresh fast even on the critical path; a
  4000×4000 layer needs to be deferred aggressively. A cost-aware
  band would defer big layers more eagerly. Probably a follow-up
  after M3 lands.
- **OQ3 — How does this interact with Geode?**
  Geode's offscreen instance model differs from tiny-skia's. The
  intrinsic-size raster works the same way (a per-layer offscreen
  device) but the GPU compose path may need scale-aware sampling
  parameters set differently. Defer Geode validation to M2.

## Future Work

- **M2C — Stop flattening bg/fg in `recomposeSplitBitmaps`.**
  *Diagnostic side delivered:* the M1 panel now iterates the unified
  paint-order tile list (background, foreground, segments, layers)
  via `snapshotCompositeTiles`, so the operator can see the
  individual tiles even while the editor blit is still going through
  the flattened bg/fg pair. *Display side still open.* Today the
  compositor pre-composes every segment + non-drag layer into two
  canvas-sized bg/fg bitmaps on every drag-target switch and uploads
  those two textures to the editor. The cached layer/segment bitmaps
  still exist (they're inputs to the recompose) but the editor never
  sees them individually. User observation while iterating M1: "we're
  re-rasterizing the fg/bg and losing the isolated promoted layers
  when this happens — it looks like we're flattening the fg/bg." The
  right architecture: editor's drag composite draws N textures
  directly (segment 0, layer 0, …, drag layer at offset, …, segment
  N), no flatten step. Eliminates a chunk of per-drag-target-switch
  work, gets rid of the asymmetric editor-display path, and unifies
  the on-screen blit with the diagnostic-panel view (the panel
  already iterates the same in-order list). Touches
  `CompositedPreview` (becomes a vector of tile entries),
  `GlTextureCache` (per-tile texture cache), `RenderPanePresenter`
  (iterates tiles), and the compositor (drops `recomposeSplitBitmaps`).
- **Tile-based caching for very large documents.** Splash is small; once
  Donner takes on multi-MB documents (illustrator-class export), the
  per-layer cache budget will pressure RAM. Tile-based caching split
  layers into `1024×1024` tiles each cached independently is the next
  scaling step. Not needed for the splash corpus.
- **Vector-side LOD.** A complex path with thousands of nodes can be
  simplified at low zoom (skip every nth point) without visible change.
  Big perf win for complex SVGs; nontrivial to land correctly. Future.
- **Geode-native rasterization path.** Geode is the target architecture
  for Donner's rendering pipeline — once it lands as the default,
  per-layer rasterization moves to GPU via the Slug algorithm, the
  compose step runs as a GPU pass over cached textures, and the cache-
  band thresholds in M3 widen significantly (GPU stretch/compose is so
  cheap that the "re-rasterize" cost drops by an order of magnitude).
  The intrinsic-size cache, scale-band logic, async preempt, and
  selection-chrome priority lane all carry over unchanged — they're
  backend-agnostic. Document the Geode-specific compose-pass design
  in a follow-up doc once the Geode backend is ready to host it.
- **Predictive prewarming during idle.** Listed in
  §Recommended Additions; implement after the core stabilizes.
