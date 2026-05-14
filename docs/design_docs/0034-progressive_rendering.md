# Design: Progressive Rendering for High-Zoom Drag-Start

**Status:** Design
**Author:** Claude Opus 4.7 (1M context)
**Created:** 2026-05-13

## Summary

After milestones M1–M9 of design doc 0033 landed, the editor still freezes for
multiple seconds on the *post-pinch-zoom drag-start* path. The
`RnrReplay_tests::DragStartAfterZoomAsyncHarnessDoesNotHang` harness measures
**6.6 s click → first drag pixel** baseline, **3.0 s with `cancelInFlight`** —
both far over the < 100 ms user-visible target from 0033's goal #1. The
irreducible cost is the rasterization of `donner_splash.svg` at ~3× canvas
(15 M pixels): the canvas-sized flat baseline + canvas-sized segment bitmaps
take ~3 s on the tiny-skia backend regardless of cancellation strategy, and
both `cancelInFlight` and "wait for in-flight to finish" pay that cost.

This doc proposes **progressive rendering**: the compositor emits an
*intermediate result* containing the just-rasterized drag-target layer (at
intrinsic size, cheap), and the editor displays it composited over the
previous canvas-sized bitmap (stretched in GL). The full-resolution canvas-
sized rasterize continues in the background and replaces the intermediate
result when it lands. The user sees the dragged element move at the right
position within ~50 ms; the rest of the scene catches up over ~3 s.

Pairs with the already-landed `cancelInFlight` (which removes the deferred-
on-`isBusy()` wait): cancellation gives a clean reset point, progressive
rendering produces an interactive frame before the slow canvas-sized work
completes.

## Goals

1. **Click → first drag-pixel < 100 ms** at any zoom on
   `donner_splash.svg` with the user's drag target visible at the new
   transform. The rest of the canvas may be stretched/blurry during the
   refinement window.
2. **Final crisp resolution lands within 1× the worker's full-render
   wall-clock**, with no observable freeze of the editor while it lands.
   Subsequent drag frames stay on the existing M2/M8 fast path.
3. **Cancellation discipline preserved.** A click during an in-flight
   intermediate render must still pre-empt cleanly via `cancelInFlight`;
   a click during the in-flight refinement must not produce a torn or
   half-refined frame.
4. **Test-gated.**
   `RnrReplayTest::DragStartAfterZoomAsyncHarnessDoesNotHang` budget
   tightens from "TBD" to **< 200 ms** for drag-2 click-to-first-pixel.
   Test must go red on the parent commit and green at the fix.

## Non-Goals

- **Tile-based partial rendering.** Splash fits in one canvas; tiling
  is a separate architecture step (see 0033 Non-Goal #4).
- **GPU-accelerated rasterization.** Geode (Slug-based GPU path
  raster) would naturally collapse this entire latency budget, but is
  not on this milestone's critical path.
- **Vector-level animation / morphing.** Static-document interaction
  only.
- **Multi-pass selection chrome.** Selection chrome already renders
  via 0033's M7 priority lane and is independent of layer rasterize.
- **Eliminating canvas-sized rasterization.** The full-resolution
  pass still runs in the background; progressive rendering just
  *unblocks the UI* from waiting for it.

## Next Steps

- **Stage 1 cut.** Modify `CompositorController::renderFrame` to
  optionally emit an intermediate result after the drag target's
  layer has been rasterized but before the canvas-sized work (flat
  baseline + segments) begins. Compose only the drag target's
  layer on top of the cached previous-canvas-size baseline.
- **Wire AsyncRenderer / RenderCoordinator** to surface the
  intermediate result as a separate `pollResult()` return, so the
  editor's GL texture cache can upload it without waiting for the
  final result.

## Implementation Plan

- [ ] **M1 — Two-stage `renderFrame` API**
  - [ ] Add `enum class RenderStage { Intermediate, Final }` to
    `RenderResult`.
  - [ ] Extend `CompositorController::renderFrame(viewport, token)` to
    accept a `StageCallback` argument that fires once after the drag-
    target layer has been rasterized + before the canvas-sized work.
  - [ ] The intermediate callback receives a `RenderResult` populated
    only with the drag-target's `compositedPreview` tile + the *prior*
    canvas-sized flat baseline (no re-rasterize).
  - [ ] Canvas-sized segments and flat baseline continue to rasterize
    after the callback; the second result emit at end of `renderFrame`
    is `RenderStage::Final`.
- [ ] **M2 — `AsyncRenderer` multi-emit**
  - [ ] Add a separate `intermediateResult_` slot and an
    `Intermediate` state in the worker FSM, distinct from `Done`.
  - [ ] `pollResult()` drains intermediates first; the final result
    overwrites the intermediate when it lands.
  - [ ] `cancelInFlight()` continues to work — it drops both
    intermediate and final results, since the user-input event that
    cancelled the render supersedes the work entirely.
- [ ] **M3 — Editor display path**
  - [ ] `RenderCoordinator::pollRenderResult` handles `Intermediate`
    by uploading the new composited preview tile (just the drag
    target) without invalidating the cached flat baseline texture.
  - [ ] `GlTextureCache::uploadComposited` already handles per-tile
    upload; verify it does the right thing when a partial preview
    arrives.
- [ ] **M4 — Harness + perf budget**
  - [ ] Extend `RnrReplayTest::ReplayRecordingAsync` to capture
    `clickToIntermediatePixelMs` separately from
    `clickToFirstDragPixelMs`.
  - [ ] Add red regression test: `clickToIntermediatePixelMs <
    200 ms` on `drag_start_hang_repro.rnr`. Must be red on the
    parent commit, green at M3.
  - [ ] Add validation: the `Final` result must arrive within
    `1.5×` of the unmodified single-stage render time (i.e. the two-
    stage split shouldn't *slow down* the full-resolution path by
    more than 50%).
- [ ] **M5 — Live verification + cleanup**
  - [ ] User runs editor against `donner_splash.svg`, performs the
    drag → pinch → drag-again repro. Confirms drag-2 click is
    responsive.
  - [ ] Strip any diagnostic logging left over from earlier
    iterations.
  - [ ] Update 0033 status table — M6 (pinch-zoom GPU stretch)
    can be deprecated in favor of this milestone; M10 (perf
    validation) is partially fulfilled by M4 here.

## Background

The harness `RnrReplayTest::DragStartAfterZoomAsyncHarnessDoesNotHang`
captures the exact timing of the user's reported lag. Headline numbers
on the unmodified post-0033-M9 codebase:

| Variant | drag2 click→first pixel | breakdown |
|---|---|---|
| Baseline (no cancel) | 6632 ms | 2984 ms deferred-on-`isBusy()` + 3648 ms in-flight prewarm finishes |
| `cancelInFlight` on deferred click | 3019 ms | 43 ms cancel + 2976 ms drag render redoes the cancelled work |

Both variants pay ~3 s for a single canvas-sized rasterize at 3× zoom on the
tiny-skia backend. Neither cancellation strategy can reduce this — the
canvas-sized work IS the cost.

The dragged element's *intrinsic-sized* layer bitmap, by contrast, takes
~10–30 ms to rasterize. The user's perception ("I clicked, where's my drag?")
is satisfied as soon as that layer is visible at the new transform. The
rest of the scene catching up to fresh resolution can happen over multiple
seconds without affecting interactivity.

Per 0033 §M2A/M2B, the compositor already rasterizes layers at intrinsic
size; the canvas-sized work is the *flat baseline* drawn by
`driver.draw(*document_)` plus the canvas-sized *segment* bitmaps emitted by
`CompositorController`. Progressive rendering exploits the fact that the
intrinsic-sized layer is already produced *first* in `renderFrame`'s
ordering — we just need to surface that as a separate result rather than
waiting for the full canvas-sized work.

## Proposed Architecture

```
Worker thread (CompositorController::renderFrame)
─────────────────────────────────────────────────
  ┌──────────────────────────────────────────────┐
  │ prepareDocumentForRendering                  │  ~50 ms (RIC rebuild)
  ├──────────────────────────────────────────────┤
  │ consumeDirtyFlags + reconcileLayers          │  ~1 ms
  ├──────────────────────────────────────────────┤
  │ rasterize drag-target layer (intrinsic-size) │  ~30 ms  ← stage 1
  ├──────────────────────────────────────────────┤
  │ emit Intermediate result (drag-target +      │
  │ STALE canvas-sized baseline from prior frame)│  StageCallback fires
  ├──────────────────────────────────────────────┤
  │ rasterize remaining intrinsic-size layers    │  ~50 ms
  │ rasterize canvas-sized segments              │  ~500 ms × N segments
  │ driver.draw (flat baseline) at new canvas    │  ~1500 ms at 3× canvas
  ├──────────────────────────────────────────────┤
  │ emit Final result                            │  return from renderFrame
  └──────────────────────────────────────────────┘
                            ▲
                            │ §M4 cancel polls remain between segments;
                            │ a cancel during stage 2 drops Final result;
                            │ Intermediate that already shipped is unaffected.
```

The editor side composites the intermediate result by:

1. Treating the new `Intermediate.compositedPreview.tiles` (containing
   just the drag-target layer at intrinsic size) as the *authoritative*
   tile for that entity.
2. Keeping the existing GL texture for the canvas-sized flat baseline,
   which now shows the scene at the *previous* canvas size, stretched
   to fit the current viewport.
3. Drawing path-overlay chrome at full crispness (0033's M7).

Net visual: the drag-target is sharp at the new transform; the rest of
the canvas is slightly stretched (1.0×→3.0× during the worst-case pinch)
for the < 3 s refinement window.

## API / Interfaces

```cpp
// donner/editor/RenderRequest.h
struct RenderResult {
  enum class Stage {
    Intermediate,  // Drag-target layer ready; canvas-sized work pending.
    Final,         // Full canvas-sized rasterize complete.
  };
  Stage stage = Stage::Final;
  // ... existing fields unchanged ...
};

// donner/svg/compositor/CompositorController.h
class CompositorController {
 public:
  using StageCallback = std::function<void(RenderResult intermediate)>;

  // Existing API: returns true on completion, false on cancel.
  bool renderFrame(const RenderViewport& viewport, CancellationToken& token);

  // New: same semantics, but `onIntermediate` fires once between the
  // drag-target layer rasterize and the canvas-sized work. The
  // callback receives a partially-populated RenderResult with stage=
  // Intermediate. Compatibility: if onIntermediate is empty, behaves
  // identically to the existing API.
  bool renderFrame(const RenderViewport& viewport, CancellationToken& token,
                   const StageCallback& onIntermediate);
};
```

The `AsyncRenderer` worker invokes the callback inline on the worker
thread. The callback grabs the result mutex, stages the intermediate
in `intermediateResult_`, and notifies via the wake callback so the UI
thread picks it up on the next `pollResult()`. The final result then
overwrites it.

`cancelInFlight()` semantics unchanged: drops whatever's currently in
flight (including a staged intermediate). The user's input event that
cancelled the render is the new authoritative state.

## Performance

| Phase | Today | With progressive |
|---|---|---|
| drag-target rasterize | ~30 ms | ~30 ms |
| Stage 1 emit + UI upload | n/a | ~10 ms (single tile upload) |
| Canvas-sized segments + flat | ~3 s | ~3 s |
| Stage 2 emit + UI upload | ~3 s + ~20 ms | ~3 s + ~20 ms |
| **User-visible click→drag-pixel** | **~3 s** | **~50 ms** |
| **Time to crisp final** | **~3 s** | **~3 s** |

Throughput is unchanged; latency-to-interactivity drops from ~3 s to
~50 ms. The "time to crisp" is identical — progressive rendering does
*not* defer the canvas-sized work, just unblocks the UI from waiting on
it.

## Testing and Validation

- **`RnrReplayTest::DragStartAfterZoomAsyncHarnessDoesNotHang`** —
  budget tightens from informational to `< 200 ms` for drag-2 click-
  to-first-pixel. Asserts the `Intermediate` result was received
  *before* the `Final`.
- **New: `RnrReplayTest::DragStartAfterZoomFinalRenderArrives`** —
  asserts the `Final` result lands within 1.5× of the existing
  baseline render time, so progressive rendering doesn't regress
  total throughput.
- **New unit test:
  `AsyncRendererTest::IntermediateAndFinalResultBothDelivered`** —
  posts a render with a `StageCallback`, polls twice, verifies both
  results arrive in order and the final supersedes the intermediate
  in the GL texture cache.
- **Live verification:** user runs the editor against
  `donner_splash.svg`, performs drag → pinch → drag-again. Reports
  the visual experience: drag-target should snap to the click
  position within ~50 ms; the rest of the canvas may refine over
  ~3 s without blocking further interaction.
- **Regression coverage on existing M2/M5/M8/M9 paths.** Run
  `bazel test //donner/editor/tests/... //donner/svg/compositor/...`
  to confirm no regression in the existing async-rendering tests.

## Alternatives Considered

1. **"Skip prewarm during pinch" (0033 §M6 / earlier proposal B).**
   Suppress canvas-sized rasterization while the canvas-commit
   debounce is active, so when the user clicks the worker is idle
   and the slow-path mouseDown fires immediately. *Rejected by
   the operator:* "I don't want to defer rendering and reduce the
   time-to-high-res." Progressive rendering achieves the same
   click responsiveness *without* deferring the high-resolution
   work — both happen, just in a sensible order.

2. **Finer-grained M4 cancellation polls inside segment / flat-
   baseline rasterize.** Would let `cancelInFlight` actually
   pre-empt a giant canvas-sized rasterize mid-stride. Doesn't
   help: even with perfect cancellation, the next render still
   has to rasterize at the new canvas size from scratch
   (~3 s). Cancellation alone doesn't change the lower bound.

3. **Low-resolution proxy rendering.** Render the whole canvas at
   ½ or ¼ scale, blit, then refine at full resolution. Simpler in
   principle but doubles the worker's total work (proxy + full),
   and the proxy still touches the full set of segments. Progressive
   rendering's "drag-target only" intermediate is much cheaper.

4. **Geode (GPU rasterization).** Would naturally collapse the
   3 s canvas-sized cost to ms. Out of scope for this milestone —
   progressive rendering benefits Geode just as much (the
   intermediate stage is still cheaper than the final), so the
   architecture isn't wasted.

## Open Questions

- **Should the intermediate result include the segment bitmaps that
  were valid at the *previous* canvas size?** Cheap to keep — they're
  already cached. The flat baseline texture is the only one that
  *needs* a fresh rasterize at the new canvas size; segments could
  potentially stretch acceptably. Worth measuring in the harness.
- **Two intermediates for very deep canvases?** If `Final` is taking
  > 5 s on huge documents, would the user benefit from a `Mid`
  stage that flushes after the first canvas-sized segment but
  before the flat baseline? Probably not at the splash's complexity;
  revisit if user reports surface a slower document.
- **Threading of `StageCallback`.** Currently invoked on the worker
  thread directly. Should it be queued back to the UI thread via the
  wake callback instead? Simpler invariant: callback never runs on
  UI thread; UI thread only sees the result via `pollResult`. Going
  with that for the initial design — the worker stages the
  intermediate via the existing mutex-protected slot mechanism.

## Future Work

- [ ] Apply progressive staging to `setDocumentMaybeStructural` (post-
  source-reparse first frame) — same root cause, same fix shape.
- [ ] Use the intermediate-result mechanism to also surface partial
  composited tiles when a per-layer rasterize takes longer than a
  budget (e.g. complex filter chains). Out of scope for this doc;
  worth revisiting once Geode lands and per-layer cost variability
  is measurable in the panel.
