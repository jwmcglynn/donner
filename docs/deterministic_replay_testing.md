# Deterministic Replay Testing {#DeterministicReplayTesting}

\tableofcontents

## Overview

The editor renders on a background `AsyncRenderer` worker thread while the UI
thread runs `EditorShell::runFrame()`. Because a worker render lands whenever it
finishes relative to wall-clock, a test that replays a recorded `.rnr` session
and compares specific frames can pass or fail by luck — the frame at which a
render becomes visible is `render_wall_clock / frame_interval`, which varies with
machine and load.

The deterministic replay framework makes the GL replay harness reproducible by
turning "when a worker result becomes visible" into a function of frame index
rather than wall-clock. It provides replay-only worker scheduling modes, a
content-only capture mode that excludes editor chrome, and a worker-delay
injection hook used to prove a test would catch a timing regression.

Guarantees callers can rely on:

- **Production behavior is unchanged at defaults.** Every knob defaults to
  today's behavior (`Realtime` scheduling, zero delay, chrome on). The real
  worker keeps running free against wall-clock; the deterministic paths are
  test-only.
- **Deterministic captures.** Under `DrainEachFrame` with `contentOnlyCapture`, a
  replayed `.rnr` produces byte-identical capture PNGs and a stable canonical
  diagnostics projection regardless of `pace`, injected worker delay, or host CPU
  load.
- **Both worker states are reproducible.** `DrainEachFrame` models a caught-up
  worker (each render lands one frame after its request); `HoldFramesBehind(n)`
  models a worker held a fixed number of frames behind.
- **All waits are bounded.** A stuck worker fails the replay with the blocking
  worker state; it can never hang CI.

## Architecture Snapshot

The dominant nondeterminism is *when a worker render becomes visible* to the
synchronous replay loop. `EditorShell::runFrame()` polls completed renders at the
top (`RenderCoordinator::pollRenderResult`) and may request a new render near the
bottom (`maybeRequestRender`).

- **`DrainEachFrame`** — before a frame reaches `pollRenderResult`, the harness
  waits until `AsyncRenderer::hasRenderInFlightForTesting()` is false. A staged
  `DoneState` result is *not* in flight, so it is left for the normal poll to
  consume. Every render then lands exactly one frame after its request, and
  UI-thread document reads happen after the worker has released document access.
- **`HoldFramesBehind(n)`** — installed at the poll seam
  (`AsyncRenderer::setReplayResultHoldFramesForTesting`), not as a harness
  afterthought, because `runFrame()` already owns `pollRenderResult`. A completed
  result is withheld for `n` poll attempts; during that window the editor's
  existing `isBusy()` checks observe the worker as busy, exactly as a slow render
  would. Releasing the gate lands the old result on a deterministic frame.
- **Content-only capture** — `EditorShell::setContentOnlyCaptureForNextFrameForReplay`
  suppresses non-document render-pane presentation for the readback frame
  (overlay texture, frame graph, selection/reference chips, pen preview, tool
  palette, context-menu affordances). It does not clear the overlay texture or
  skip overlay rasterization, so the next normal frame is unaffected.
- **Worker-delay injection** —
  `AsyncRenderer::setReplayRenderDelayForTesting(delay)` makes the worker sleep a
  fixed duration per render, outside any mutex-held region. It exists so a
  deterministic test can demonstrate it would catch the timing bug; the suite
  keeps no permanent "realtime must fail" assertion.

A separate non-GL harness (`donner/editor/tests/RnrReplay_tests.cc`) is
deterministic by construction: it reimplements a simplified loop where
`SyncCanvasSizeDebounced` keys the 120 ms canvas-commit throttle on each frame's
recorded `timestampSeconds` (a virtual clock) and drives the worker synchronously.
The GL harness cannot reuse that simplified loop because its value *is* fidelity
to the real `runFrame()` + GL texture path; this framework brings equivalent
determinism to the real loop.

## API Surface

Replay-and-capture entry point (`donner/editor/repro/GlRnrReplay.h`):

```cpp
bool RunGlRnrReplay(const GlRnrReplayOptions& options, GlRnrReplayResult* result,
                    std::string* error);
```

`GlRnrReplayOptions` (deterministic-replay fields; defaults reproduce production
behavior):

| Field | Default | Meaning |
|-------|---------|---------|
| `workerScheduling` | `Realtime` | `Realtime` / `DrainEachFrame` / `HoldFramesBehind` |
| `holdFramesBehind` | `0` | Frames to withhold each result; requires `HoldFramesBehind` |
| `workerRenderDelayMsForTesting` | `0` | Fixed per-render worker delay (ms) |
| `contentOnlyCapture` | `false` | Suppress non-document chrome on capture frames |
| `cropMode` | `Full` | `Full` / `RenderPane` / `DocumentCanvas` |
| `pace` | `true` | Pace replay by recorded timestamps |

Invalid combinations are rejected by `RunGlRnrReplay` (returns `false` with an
error): a negative `holdFramesBehind` or `workerRenderDelayMsForTesting`, or a
non-zero `holdFramesBehind` without `HoldFramesBehind` scheduling.

`GlRnrReplayResult` carries `captures`, per-frame `frameDiagnostics`
(`GlRnrReplayFrameDiagnostics`, including the scheduling mode, injected delay,
held-frame count, and whether a result was withheld), and
`finalSelectedElementLabel`.

Worker-state hooks (`donner/editor/AsyncRenderer.h`, all test-only, no-ops at
defaults):

```cpp
bool hasRenderInFlightForTesting() const;              // Rendering/Cancelling only, not Done
bool waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::time_point deadline);
void setReplayRenderDelayForTesting(std::chrono::milliseconds delay);  // negative clamped to 0
void setReplayResultHoldFramesForTesting(int frameCount);
std::uint64_t replayResultHoldPollCountForTesting() const;
```

`hasRenderInFlightForTesting()` is deliberately narrower than `isBusy()`: a staged
`DoneState` result is busy for normal request scheduling but is *not* in flight,
so a drain that waited on `isBusy()` would hang forever on a staged result.

## Testing and Observability

- **`//donner/editor/tests:async_renderer_tests`** — pins the
  `Rendering`/`Cancelling`/`Done` distinctions, default-zero delay, and the hold
  semantics.
- **`//donner/editor/tests:gl_rnr_replay_tests`** — the end-to-end GL replay
  coverage. `DrainEachFrameContentCaptureIsDeterministicAcrossPaceAndDelay` runs a
  focused fixture over the `{pace on/off} × {0,5,10,20,50 ms}` matrix and asserts
  byte-identical captures plus a stable diagnostics projection;
  `ContentOnlyDocumentCanvasCaptureMatchesRendererGroundTruth` checks content-only
  readback against a cropped `svg::Renderer::draw` ground truth;
  `HoldFramesBehindRecordsWithheldReplayDiagnostics` pins the hold semantics.
- **`//donner/editor/tests:editor_layer_stress_tests`** — ConcurrentDom safety:
  busy-gated clicks, cancellation, structural remap, and UI reads under
  deterministic worker control.
- **`//donner/editor/tests:rnr_replay_tests`** — the non-GL deterministic harness.

Capture comparisons use `bitmap_golden_compare` + pixelmatch identity params, not
percentage thresholds. Diagnostics comparisons use the `CanonicalReplayDiagnostics`
test helper, which omits fields that are per-process identity rather than replay
invariants (backend texture handles, worker wall-clock durations) so they cannot
create false flakes; raw diagnostics remain available for failure output.

## Maintainer Checklist

- Keep the deterministic knobs test-only. Defaults must reproduce production
  behavior; the real worker runs free against wall-clock.
- Every replay wait must be bounded by a deadline and report the worker state that
  blocked it.
- `DrainEachFrame` may only change *when* staged results become visible — never
  which frames request renders or the drag/presentation state machine.
- `HoldFramesBehind` must preserve `maybeRequestRender` semantics: a withheld
  result looks exactly like a slow in-flight render to the editor.
- Content-only capture is a presenter/readback mode: it must not clear overlay
  textures, skip overlay rasterization, or perturb the next non-capture frame.
- Do not add a permanent test that asserts the realtime path is flaky; the
  red→green evidence lives in commit history or a local witness run.

## Limitations and Future Extensions

- This is not a general virtual clock for the whole editor. It controls the
  dominant nondeterministic input (worker landing) plus the canvas-commit throttle
  the affected tests touch; ImGui animation timing is not virtualized.
- Geode GPU replay targets require a working GPU; they are validated on CI (the
  Intel Arc + Mesa dev box hits an environmental failure, issue #542).
- Recording per-render virtual durations into the `.rnr` format is deferred unless
  the synthetic scheduling modes prove unfaithful.
