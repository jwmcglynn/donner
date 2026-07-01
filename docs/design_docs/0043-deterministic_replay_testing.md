# Design: Deterministic Multi-Thread Replay Testing

**Status:** Implemented — shipped in [#602](https://github.com/jwmcglynn/donner/pull/602),
closing [#601](https://github.com/jwmcglynn/donner/issues/601). The `#601`-disabled
`gl_rnr_replay_tests` / `editor_layer_stress_tests` subtests are re-enabled and run
untagged (default `bazel test //...`, not flaky-quarantined). No standalone developer
doc has been written yet; this design doc remains the living reference for the
deterministic-replay mechanism.
**Author:** Claude Opus 4.7
**Created:** 2026-05-23

## Summary

The GL replay test harness (`donner/editor/repro/GlRnrReplay.cc`, exercised by
`donner/editor/tests/GlRnrReplay_tests.cc`) drives recorded `.rnr` sessions
through the **real** `EditorShell::runFrame()` loop while the editor renders on a
background `AsyncRenderer` worker thread. Today it paces frames against
wall-clock time (`pace=true`), so the frame at which a worker render *lands*
depends on `render_wall_clock / frame_interval` — which varies with machine and
load. Tests that compare two specific captured frames, or that assert the worker
is "behind" at a specific frame, therefore pass or fail by luck.

This made `gl_rnr_replay_tests` chronically flaky on CI (~50% failure across
every branch, including `main`). The first two subtests disabled under
[#601](https://github.com/jwmcglynn/donner/issues/601) were
`SecondDragActiveFrameMatchesMouseUpFrame` and
`ZoomOutDragDoesNotPublishNewOverlayOverStaleSplitTiles`; later related disables
also accumulated in `GlRnrReplay_tests.cc` and `EditorLayerStress_tests.cc` where
the async worker races UI-thread document reads or lands presentation results at
machine-dependent frames.

This doc proposes a **deterministic replay framework**: replay-only worker
scheduling modes that make render landing a function of frame index, not
wall-clock; a content-only readback mode that excludes render-pane/editor chrome;
and delay hooks used purely to validate determinism. It also separates two bug
classes that #601 currently groups together:

- **Presentation timing nondeterminism:** a correct result lands on the wrong
  replay frame.
- **Concurrent DOM access nondeterminism:** the UI thread reads live document
  state while the worker has temporarily placed the document in
  `ThreadingMode::ConcurrentDom`.

Both have to be addressed before the disabled multithreaded tests can be
re-enabled safely.

This is **test-infrastructure only**. It must not change how the editor schedules
or renders for real users.

## Goals

- **Deterministic captures.** A replayed `.rnr` produces byte-identical capture
  PNGs and identical per-frame diagnostics regardless of `pace=true`/`pace=false`,
  injected worker delay, or host CPU load.
- **Both worker states reproducible.** Support deterministic "worker caught up"
  (every render lands one frame after its request) *and* "worker N frames behind"
  (results held a fixed number of frames) so tests that need either can be
  written without racing wall-clock.
- **Chrome-free content capture.** Allow a capture that contains only rendered
  SVG document content, excluding render-pane overlays, frame graphs, selection
  chips, tool palettes, and other editor chrome, so content-alignment assertions
  don't fail on intentional chrome transitions.
- **Concurrent DOM read safety.** Make replay and stress harnesses either drain
  the worker before UI-thread live-document reads or exercise those reads through
  explicit read guards. Deterministic timing must not hide a real unsafe access.
- **A validation contract.** Provide a worker-delay injection hook so each
  deterministic test can prove (red→green) that it would have caught the timing
  bug, and a determinism assertion utility that runs a replay under several
  delay/pace settings and asserts identical output.
- **Re-enable the #601-related disabled tests** as deterministic, meaningful
  tests where the end-to-end path still adds coverage; delete or keep disabled
  only the GL variants whose invariant is already covered by narrower tests.

## Non-Goals

- **No change to production editor behavior.** The deterministic scheduling,
  delay injection, and chrome suppression are replay/test-only paths, gated by
  options that default to today's behavior. The real `AsyncRenderer` worker keeps
  running free against wall-clock.
- **Not a general virtual-clock for the whole editor.** We do not virtualize
  every wall-clock dependency (e.g. ImGui animations). We control the dominant
  one (worker landing) plus the canvas-commit throttle the affected tests touch.
- **Not a license to compare unstable diagnostics.** Backend texture handles,
  worker wall-clock durations, and other per-process identifiers are diagnostics,
  not deterministic invariants. The deterministic comparison must canonicalize
  or omit those fields.
- **Not changing the `.rnr` file format** in v1 (recording per-render durations
  into `.rnr` is listed as a future option, not a requirement).
- **Not re-litigating the editor's drag/commit presentation.** The active-drag
  selection-chrome settle that surfaced during investigation is intentional
  (confirmed by the editor owner); the framework works around it, it does not
  "fix" it.

## Next Steps

- Land the replay-only worker state probes and delay hook first. Keep them
  unavailable from public editor APIs and assert their default behavior in
  `//donner/editor/tests:async_renderer_tests`.
- Implement `DrainEachFrame` for GL replay and prove `SecondDrag` is identical
  across pace/delay settings with content-only readback.
- Decide each #601 disabled test individually: re-enable it when the deterministic
  end-to-end path adds coverage, or delete/leave disabled the GL variant when a
  narrower test already pins the invariant better.

## Implementation Plan

- [x] Milestone 1: Worker state probes and delay hook (test-only)
  - [x] Add `AsyncRenderer::hasRenderInFlightForTesting()` (true for
    `RenderingState`/`CancellingState`, false for `DoneState`) so replay can
    distinguish "worker still touching the document" from "result staged".
  - [x] Add a bounded `waitUntilNoRenderInFlightForTesting(timeout)` or equivalent
    polling helper. A stuck worker must fail the replay with a clear error rather
    than hot-spin forever.
  - [x] Add `setReplayRenderDelayForTesting(std::chrono::milliseconds)`, default
    zero. The delay must not run under `AsyncRenderer::mutex_`; if it runs while
    document access is held, that must be deliberate and covered by a separate
    ConcurrentDom stress test.
  - [x] Add unit coverage in `//donner/editor/tests:async_renderer_tests` for the
    `Rendering`/`Cancelling`/`Done` distinctions and for default-zero delay.
- [x] Milestone 2: Deterministic result scheduling in GL replay
  - [x] Add `GlRnrReplayOptions::workerScheduling` = `{ Realtime, DrainEachFrame,
    HoldFramesBehind }`, defaulting to `Realtime`, plus `holdFramesBehind` and
    `workerRenderDelayMsForTesting`.
  - [x] Implement `DrainEachFrame` by waiting before `shell.runFrame()` reaches
    `RenderCoordinator::pollRenderResult`, so any staged result lands on that
    replay frame and UI-thread document reads happen after worker document access
    is released.
  - [x] Implement `HoldFramesBehind` at the poll seam, not as an external harness
    afterthought. `EditorShell::runFrame()` owns `pollRenderResult`, so the hold
    gate must live in `RenderCoordinator` or `AsyncRenderer::pollResult()` test
    policy and intentionally preserve the same "worker is busy" behavior that a
    slow render would expose to `maybeRequestRender`.
  - [x] Add replay diagnostics that record the scheduling mode, injected delay,
    held-frame count, and every frame where a result is intentionally withheld.
- [x] Milestone 3: Content-only readback
  - [x] Add `GlRnrReplayOptions::contentOnlyCapture`, default false. Apply it
    only to capture frames unless a test explicitly requests all frames.
  - [x] Suppress presentation of non-document render-pane chrome for that frame:
    overlay texture, frame graph, selection size chip, reference highlight chip,
    tool palette, pen preview, and context-menu affordances. Do not suppress
    overlay rasterization or mutate overlay caches; this is a readback/presenter
    mode, not state.
  - [x] Verify a selected static scene's content-only GL readback matches
    `svg::Renderer::draw` ground truth under
    `//donner/editor/tests:gl_rnr_replay_tests`.
- [x] Milestone 4: Determinism validation contract
  - [x] Add a helper that runs a replay under a bounded matrix of pace and delay
    settings. Use the full `{pace on/off} x {0,5,10,20,50 ms}` matrix for one
    focused fixture; keep per-test matrices small enough for normal CI.
  - [x] Compare byte-identical captures and a canonical diagnostics subset. Omit
    texture handles, worker milliseconds, and any field whose value is an
    allocator/backend identity rather than a replay invariant.
  - [x] Satisfy the red->green rule in commit history: first land or locally run
    the failing assertion on the broken scheduling path, then switch it to the
    deterministic mode. Do not commit a permanent negative test that expects the
    realtime path to be flaky.
- [x] Milestone 5: Re-enable or retire #601 disabled tests
  - [x] Re-enable `SecondDragActiveFrameMatchesMouseUpFrame` as
    `DrainEachFrame` + `contentOnlyCapture`, with injected-delay coverage.
  - [x] Re-evaluate `ZoomOutDragDoesNotPublishNewOverlayOverStaleSplitTiles`:
    retired the GL variant because the stale split-tile guard is already covered
    by narrower `RenderCoordinatorTest` / `AsyncRenderer_tests.cc` coverage, while
    `HoldFramesBehindRecordsWithheldReplayDiagnostics` pins the replay hold
    semantics.
  - [x] Re-enable or retire the GL replay tests disabled for ConcurrentDom access.
    `ReplaysSourcePaneCharacterInput`, `GeodeDragZoomOReplayCoversTextureReuseWindow`,
    and `FilteredElementOThenRDragDoesNotPopOBackOnRClick` are re-enabled;
    `ClickAfterZoomBeforeRerasterSelectsNewTarget` is retired in favor of the
    re-enabled non-GL `DragStartAfterZoomAsyncHarnessDoesNotHang` liveness test.
  - [x] Re-enable `EditorLayerStressTest` cases by adding deterministic worker
    control to the stress harness, not by relying on wall-clock luck.

## Background

- Issue [#601](https://github.com/jwmcglynn/donner/issues/601) tracks this work
  and records the full investigation.
- The non-GL replay harness `donner/editor/tests/RnrReplay_tests.cc` is already
  deterministic by reimplementing a simplified loop: `SyncCanvasSizeDebounced`
  keys the 120 ms canvas-commit throttle on each frame's recorded
  `timestampSeconds` (a virtual clock), and `RequestRenderAndWait` drives the
  worker synchronously. The GL harness can't reuse that simplified loop because
  its value *is* fidelity to the real `EditorShell::runFrame()` + GL texture
  path; this design brings equivalent determinism to the real loop.
- Prototyping during #600 established the key facts this design rests on:
  - Injecting a fixed per-render worker delay reproduces the exact CI failure
    (`SecondDrag` → 3420 px, matching CI), giving a deterministic local repro.
  - A drain-before-poll change made `SecondDrag` produce an **identical** result
    across injected delays of 0/5/10/20/50 ms and every run — i.e. fully
    deterministic — confirming the scheduling approach.
  - With timing removed, the residual `SecondDrag` diff is entirely the editor
    selection chrome (the active-drag transform-preview box vs the settled
    committed box), which settles intentionally after the drag — hence the need
    for chrome-free content capture rather than a "fix".

## Disabled Test Inventory

The #601-related disabled tests have been handled as follows:

- `GlRnrReplayTest.SecondDragActiveFrameMatchesMouseUpFrame` is re-enabled with
  `DrainEachFrame`, injected delay coverage, and content-only document-canvas
  capture.
- `GlRnrReplayTest.ZoomOutDragDoesNotPublishNewOverlayOverStaleSplitTiles` is
  retired. The stale split-tile publication guard is pinned by narrower
  `RenderCoordinatorTest` / `AsyncRendererTest` coverage; the GL replay variant
  was only a wall-clock-dependent way to create the stale window.
- `GlRnrReplayTest.ReplaysSourcePaneCharacterInput`,
  `GeodeDragZoomOReplayCoversTextureReuseWindow`, and
  `FilteredElementOThenRDragDoesNotPopOBackOnRClick` are re-enabled under
  deterministic worker draining.
- The disabled `EditorLayerStressTest`, `StructuredEditingStressTest`, and
  non-GL `RnrReplayTest.DragStartAfterZoomAsyncHarnessDoesNotHang` cases are
  re-enabled with deterministic drains or liveness assertions instead of
  host-dependent wall-clock budgets.
- `GlRnrReplayTest.ClickAfterZoomBeforeRerasterSelectsNewTarget` is retired: once
  deterministic worker draining is applied it passes, but it costs multiple
  minutes locally and duplicates the liveness coverage now carried by the non-GL
  replay harness.

This leaves no #601-related disabled tests in `donner/editor/tests`.

## Adversarial Review / Premortem

**Failure mode: `Done` is mistaken for active worker access.** Today
`AsyncRenderer::isBusy()` intentionally treats `DoneState` as busy so the UI does
not post another request before the staged result is consumed. The replay drain
must not reuse that predicate: draining until `isBusy() == false` would wait
forever on a staged result that needs the next `pollResult()` to land. Mitigation:
add a separate `hasRenderInFlightForTesting()` contract and pin it in
`//donner/editor/tests:async_renderer_tests`.

**Failure mode: `HoldFramesBehind` is bolted on where it cannot work.** The GL
harness calls `EditorShell::runFrame()`, and `runFrame()` owns
`RenderCoordinator::pollRenderResult()`. A replay-side counter outside that poll
seam cannot withhold a result that `runFrame()` has already consumed. Mitigation:
put the hold gate at the `RenderCoordinator`/`AsyncRenderer::pollResult()` seam
and log every intentionally withheld frame. Treat the resulting `DoneState` as
intentionally busy, because that matches a slow worker from `maybeRequestRender`'s
point of view.

**Failure mode: content-only capture still includes editor UI.** The current
`DocumentCanvas` crop can include overlay textures, frame graph, chips, and tool
palette drawing if they overlap the document canvas. Suppressing only selection
AABBs is too narrow. Mitigation: make content-only capture a render-pane
presenter/readback mode that skips every non-document draw for the capture frame
without mutating overlay caches or editor state.

**Failure mode: diagnostics comparison makes nondeterminism worse.** Texture
handles and worker durations are useful debugging fields but are not stable
across runs or backends. A helper that compares whole diagnostics structs will
create false flakes. Mitigation: compare a named canonical diagnostics projection
and leave raw diagnostics for failure output.

**Failure mode: the delay hook creates the race it is supposed to diagnose.** A
worker sleep while holding `AsyncRenderer::mutex_` or document write access can
turn a timing validation knob into a deadlock or a new ConcurrentDom exposure.
Mitigation: keep the normal render-delay hook outside mutex-held regions; use a
separate explicitly named access-window stress hook only if a test needs to widen
the ConcurrentDom window.

**Failure mode: `DrainEachFrame` hides busy-worker bugs.** A caught-up worker is
useful for content identity, but it does not exercise user input arriving while a
render is still busy. Mitigation: keep direct `AsyncRenderer`/stress coverage for
busy-gated clicks, cancellation, stale split tiles, and structural remap. Do not
delete those invariants just because the GL replay path becomes deterministic.

**Failure mode: the determinism matrix is too expensive for normal CI.** Ten GL
replays per assertion can make `bazel test //...` impractical and encourage
future disables. Mitigation: run the full matrix on one high-value fixture, use
small per-test matrices, and keep all targets in the normal
`//donner/editor/tests:gl_rnr_replay_tests` / `//donner/editor/tests:editor_layer_stress_tests`
CI surface unless a target is explicitly tagged slow.

## Requirements and Constraints

- Production behavior must be unchanged when the new options are at their
  defaults (`Realtime`, delay 0, chrome on). Any default-path overhead is limited
  to cheap test-knob reads. Enforced by
  `//donner/editor/tests:async_renderer_tests` and the default-option smoke paths
  in `//donner/editor/tests:gl_rnr_replay_tests`.
- All replay waits must be bounded by timeout and report the worker state that
  blocked progress. A stuck worker can fail a replay, but it can never hang CI.
  Enforced by `//donner/editor/tests:gl_rnr_replay_tests`; a broken wait causes
  that target to fail by timeout or explicit replay error.
- Capture comparisons use the existing `bitmap_golden_compare` + pixelmatch
  identity params rather than bespoke percentage thresholds. Enforced by
  `//donner/editor/tests:gl_rnr_replay_tests` and
  `//donner/editor/tests:editor_layer_stress_tests`, which fail on non-identical
  replay captures unless a test names an explicit approved tolerance.
- `DrainEachFrame` must not perturb which frames request renders or mutate the
  drag/presentation state machine; it may only change when staged worker results
  are visible to the frame loop. Enforced by
  `//donner/editor/tests:gl_rnr_replay_tests`, especially
  `DrainEachFrameContentCaptureIsDeterministicAcrossPaceAndDelay`.
- `HoldFramesBehind` must preserve `maybeRequestRender` semantics: while a result
  is intentionally withheld, the editor observes the worker as busy exactly as it
  would during a slow render. Enforced by
  `//donner/editor/tests:async_renderer_tests` and
  `//donner/editor/tests:gl_rnr_replay_tests`.
- Content-only capture is a presentation/readback option only. It must not clear
  overlay textures, skip overlay rasterization, or change the next non-capture
  frame. Enforced by `//donner/editor/tests:gl_rnr_replay_tests` and presenter
  coverage in `//donner/editor/tests:editor_layer_stress_tests`.

## Proposed Architecture

The dominant nondeterminism is **when a worker render becomes visible** relative
to the synchronous replay loop. `EditorShell::runFrame()` polls completed renders
at the top (`RenderCoordinator::pollRenderResult`) and may request a new render
near the bottom (`maybeRequestRender`). With `pace=true` the worker gets a
variable amount of wall-clock between frames, so a render requested at frame N
lands at frame N+k for a load-dependent k.

```mermaid
flowchart LR
  subgraph Replay loop (UI thread)
    A[frame N: pollResult] --> B[process input] --> C[maybeRequestRender]
  end
  subgraph Worker (other thread)
    W[render ...] -->|wall-clock| D[Done]
  end
  C -. request .-> W
  D -. polled at frame N+k .-> A
```

**`DrainEachFrame`** removes the variability: before `shell.runFrame()`
can reach `RenderCoordinator::pollRenderResult`, the harness waits until
`hasRenderInFlightForTesting()` is false. `DoneState` does not count as in
flight, so a staged result is left for the normal poll to consume. Every render
then lands exactly one frame after its request — a deterministic "fast worker
with one-frame latency" — independent of how long the render took. This matches
the behavior fast machines already exhibit where the timing-only disabled tests
usually pass.

**`HoldFramesBehind(n)`** covers tests that must observe a worker behind the UI
thread, e.g. mid-zoom re-rasterization. It cannot be implemented as a wrapper
after `runFrame()` because `runFrame()` already consumed the result. Instead the
replay installs a test-only gate at the poll seam. When a result reaches
`DoneState`, `pollResult()` returns `std::nullopt` for `n` replay frames, leaving
the editor's existing `isBusy()` checks to observe the worker as busy. Releasing
the gate makes the old result land on a deterministic frame.

**Content-only capture** addresses comparisons where editor chrome legitimately
differs between two captured frames. `EditorShell` gains a replay capture mode
that suppresses non-document render-pane presentation for the readback frame:
overlay texture, frame graph, selection/reference chips, pen preview, tool
palette, and context-menu affordances. It does not clear the overlay texture or
skip overlay rasterization, so the next normal frame is unaffected.

**Worker-delay injection** (`setReplayRenderDelayForTesting`) makes the worker
sleep a fixed duration per render. It exists solely so a deterministic test can
prove it would catch the timing bug. The committed test suite should not keep a
permanent "realtime must fail" assertion; the red->green evidence comes from the
commit sequence or a local witness run on the broken scheduling path.

## API / Interfaces

```cpp
// AsyncRenderer.h — replay/test-only additions (no-ops at defaults)
bool hasRenderInFlightForTesting() const;  // Rendering/Cancelling only, not Done
bool waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::time_point deadline);
void setReplayRenderDelayForTesting(std::chrono::milliseconds delay);
void setReplayResultHoldFramesForTesting(int frameCount);

// GlRnrReplay.h
enum class WorkerScheduling { Realtime, DrainEachFrame, HoldFramesBehind };
struct GlRnrReplayOptions {
  // ... existing ...
  WorkerScheduling workerScheduling = WorkerScheduling::Realtime;
  int holdFramesBehind = 0;
  int workerRenderDelayMsForTesting = 0;
  bool contentOnlyCapture = false;
};

// Test helper (donner/editor/tests/gl_rnr_replay_tests)
std::string CanonicalReplayDiagnostics(const GlRnrReplayResult& result);
```

## Data and State / Concurrency

- The replay delay is an `std::atomic<std::chrono::milliseconds::rep>` read once
  per worker iteration; zero by default so production sees a single relaxed load.
- `hasRenderInFlightForTesting()` reads the existing worker-state variant under
  the existing mutex. It is deliberately narrower than `isBusy()`: `DoneState` is
  not in flight, but remains busy for normal editor request scheduling.
- `HoldFramesBehind` state lives at the result-poll seam. While the gate is
  holding a result, the worker remains in `DoneState`; this intentionally blocks
  `maybeRequestRender` the same way a slow in-flight render would.
- Content-only capture state is scoped to a replay frame and never mutates
  `GlTextureCache`, overlay bitmaps/textures, or selection state.
- All waits use a timeout and include the latest worker state in the error.

## Security / Privacy

Replay files can embed SVG source and recorded editor input. The deterministic
framework does not add new file formats or network access. New diagnostics should
stay in the existing test output directories and must not print full embedded SVG
source unless the current replay tools already do so for that failure mode.

The new ConcurrentDom tests are safety tests: they should make illegal UI-thread
access fail deterministically in debug builds and avoid converting a scoped-access
assert into an unchecked release-build use-after-free.

## Testing and Validation

- **Worker-state tests.** `//donner/editor/tests:async_renderer_tests` asserts
  that `hasRenderInFlightForTesting()` is true only while the worker may still be
  touching the document, and false for staged `DoneState` results.
- **Determinism matrix.** `DrainEachFrameContentCaptureIsDeterministicAcrossPaceAndDelay`
  runs a focused replay under `{pace on/off} x {0,5,10,20,50 ms}` and asserts
  byte-identical captures plus a stable canonical diagnostics projection.
- **Content-only capture.** `ContentOnlyDocumentCanvasCaptureMatchesRendererGroundTruth`
  verifies that static-scene readback excludes overlay/chrome and matches a
  cropped `svg::Renderer::draw` ground truth.
- **ConcurrentDom safety.** `//donner/editor/tests:editor_layer_stress_tests`
  covers busy-gated clicks, cancellation, structural remap, and UI reads while
  the worker is controlled deterministically.
- **Red->green per test.** The red step is recorded by landing or locally running
  the failing assertion on the broken path before switching to deterministic
  scheduling. CI should contain only stable positive assertions.
- **Local GPU caveat.** Geode GPU targets fail on the Intel Arc + Mesa dev box
  (issue #542, environmental); validate Geode coverage on CI.

## Open Questions

- Should the 120 ms canvas-commit throttle
  (`RenderCoordinator::kCanvasSizeCommitDelay`) be routed through a replay
  virtual clock, or is worker landing the only nondeterministic input for the
  tests we re-enable? Add throttle virtualization only when a test proves it is
  needed.
- Which UI-thread live-document reads need production read guards versus replay
  drains? The answer should come from the disabled ConcurrentDom tests, not from
  broad speculative locking.
- Is recording per-render virtual durations into `.rnr` ever worth the format
  change? Deferred unless the synthetic modes prove unfaithful.

## Reversibility

The runtime changes are behind defaults that preserve production behavior.
Reverting removes the replay options, delay/hold hooks, content-only capture
mode, and the re-enabled tests. If review decides a GL replay variant is
redundant with narrower coverage, deleting that dead test is part of the change
rather than a rollback hazard. No production data or `.rnr` format changes.
