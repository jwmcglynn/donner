# Design: UI input recording + replay (`.donner-repro` files)

**Status:** Implementing (Stage 1 landed — recording; Stage 2 — headless
replay — scoped)
**Author:** Claude Opus 4.7
**Created:** 2026-04-19

## Summary

Capture the editor's raw ImGui input state every frame, serialize to a
line-oriented JSON file, and (Stage 2) replay that recording against a
headless editor with a golden PNG of the render pane as the correctness
gate.

The central insight: recording at the **raw ImGui input** boundary —
below menu action dispatch, below tool dispatch, below the compositor
— means a replay exercises every layer of the stack. A user-visible
bug you can reproduce by clicking and dragging in the editor *always*
reproduces in playback, regardless of whether it lives in the
renderer, the compositor, the RIC, the DOM, the event bridge, or
somewhere in between.

Recording at a higher level (`SelectTool::onMouseDown/Move/Up`, say)
is cleaner to implement but skips everything the ImGui layer
handles before dispatching to tools: menu bar clicks, keyboard
shortcuts, sidebar widgets, text editor typing, pinch gestures, focus
changes, window resizes. A bug triggered by an unusual sequence of
those is invisible to a tool-level recording. Going to the input
boundary costs a couple hundred LOC of recorder code but gives full
coverage forever.

## Goals

- **One-flag recording.** `donner-editor --save-repro <path> input.svg`
  captures everything the user does until the editor exits.
- **Human-readable file format.** A developer can `cat` a
  `.donner-repro`, spot what the user did, and hand-edit to trim or
  annotate. No protobuf, no binary wire format.
- **Zero new external deps on the editor binary.** The recorder uses a
  hand-rolled NDJSON emitter; the (test-only) player is free to pull
  in a real JSON parser later if needed.
- **Atomic file writes.** A crash mid-flush never truncates an existing
  file. The recorder writes to `<path>.tmp` and renames.
- **(Stage 2) Headless replay into a golden-PNG-compared test.** Given
  a `.donner-repro`, a test replays it, snapshots the render-pane
  bitmap at a configurable frame, and asserts byte-identity (within
  premul tolerance) against a committed golden.

## Non-Goals

- Capturing rendering commands at the `RendererInterface` level —
  the existing `SerializingRenderer` / `.rnr` path (see
  `donner/editor/sandbox/`) already covers draw-call-level recording.
  The two mechanisms are complementary: `.rnr` captures "what did the
  renderer do", `.donner-repro` captures "what did the user do".
- Cross-session playback. A recording is paired with a specific SVG
  file (path stored in metadata). The player resolves that file at
  replay time; if the SVG has changed, playback is undefined.
- Capturing system-level state (fonts, monitor DPI, GL driver). The
  recording is ImGui-input-plus-session-metadata; backend-level
  differences between record-time and replay-time are the replayer's
  problem, not the recorder's.
- Network / collaborative editing replay. Current scope is local-only.

## Trust Boundary

A `.donner-repro` file is user-generated content. A malicious file
could attempt to:

1. **Overflow buffers** on parse. Mitigated: the parser uses
   `std::string_view` + bounds-checked indexing; all numbers go
   through `std::strtod` which handles arbitrary-length input
   gracefully. No `scanf`, no `sprintf`, no fixed char buffers in the
   parsing path.
2. **Cause resource exhaustion** by declaring a huge number of
   events. Mitigated upstream: the player (Stage 2) caps frame count
   at a configurable limit before starting playback.
3. **Escape the output path** via a path-traversal `svgPath`. Not a
   concern in the recorder (the recorder only writes to the user's
   chosen `--save-repro` path, not to the `svgPath`). The player
   resolves `svgPath` relative to its own working directory; callers
   should pass in a sandboxed working directory if running untrusted
   recordings.

The recorder is trusted (runs in the editor that the user already
runs with full privileges). The player, when eventually built, is
less trusted because it may run against user-supplied files in CI
or shared infrastructure. Scope that risk at Stage 2.

## File Format

One JSON object per line (NDJSON). First line is session metadata;
each subsequent line is one frame.

**Metadata line** (always first):

```json
{"v":1,"svg":"donner_splash.svg","wnd":[1600,900],"scale":2.0,"exp":0,
 "at":"2026-04-19T12:34:56Z"}
```

| Field | Type | Meaning |
|-------|------|---------|
| `v` | int | File format version (currently `1`; reader rejects mismatch) |
| `svg` | string | Path to the SVG that was being edited |
| `wnd` | `[int,int]` | Initial logical window size `[w, h]` |
| `scale` | float | HiDPI display scale factor |
| `exp` | `0\|1` | `--experimental` flag state |
| `at` | string | ISO-8601 wall-clock timestamp (informational) |

**Frame line**:

```json
{"f":42,"t":0.701,"dt":16.7,"mx":823.5,"my":412.25,"btn":1,"mod":0,
 "e":[{"k":"mdown","b":0}]}
```

| Field | Type | Meaning |
|-------|------|---------|
| `f` | int | Monotonic frame index (0-based) |
| `t` | float | Seconds since recording started |
| `dt` | float | `ImGuiIO::DeltaTime` this frame, in **milliseconds** |
| `mx`, `my` | float | Mouse position in logical window coordinates |
| `btn` | int | Bitmask of held mouse buttons (bit N = button N down) |
| `mod` | int | Modifier bitmask: `1<<0`=Ctrl, `1<<1`=Shift, `1<<2`=Alt, `1<<3`=Super |
| `e` | `[Event, …]` | Optional list of discrete events that fired during this frame |

**Discrete event kinds** (tag in `k` field):

| `k` | Meaning | Fields |
|-----|---------|--------|
| `mdown` | mouse button down | `b` (button index) |
| `mup` | mouse button up | `b` |
| `kdown` | keyboard key down | `key` (ImGuiKey int), `m` (modifiers) |
| `kup` | keyboard key up | `key`, `m` |
| `chr` | character typed | `c` (UTF-32 code point) |
| `wheel` | mouse wheel scroll | `dx`, `dy` (float deltas) |
| `resize` | window resize | `w`, `h` |
| `focus` | window focus change | `on` (0 or 1) |

The frame record carries the **continuous** state (mouse pos, button
mask); the discrete event list captures transitions that can't be
reconstructed from the continuous signal (key presses, char input,
wheel ticks). A replayer that drops a discrete event on the floor
self-corrects on the next frame — the continuous state wins — so the
format is resilient to benign corruption or interpreter bugs.

## Stage 1 — Recording (landed)

### Architecture

```
main loop
├─ waitEvents()              ← GLFW blocks for user input
├─ window.beginFrame()       ← ImGui_ImplGlfw_NewFrame populates ImGuiIO
│                              from GLFW events; ImGui::NewFrame() runs
├─ shell.runFrame()
│   ├─ reproRecorder_->snapshotFrame()   ← HOOK POINT (first line)
│   ├─ (menu bar, panes, tools, rendering, …)
│   └─
└─ window.endFrame()
```

The recorder runs exactly once per frame, at the very top of
`EditorShell::runFrame`, immediately after ImGui has populated its IO
state from GLFW and before any widget has consumed it. It reads:

- `io.MousePos` → frame's `mx, my`
- `io.MouseDown[]` → button mask, diffed against previous frame to
  emit `mdown`/`mup` events
- `io.KeyCtrl/Shift/Alt/Super` → modifier bitmask
- `io.MouseWheel / MouseWheelH` → wheel deltas (emitted as discrete
  `wheel` event when non-zero)
- `ImGui::IsKeyPressed/Released` for a curated watchlist of keys →
  `kdown`/`kup` events
- `io.InputQueueCharacters` → `chr` events
- `io.DisplaySize` vs previous-frame size → `resize` event
- `io.AppFocusLost` vs previous-frame focus state → `focus` event

All bookkeeping lives in the recorder's own private prev-frame state;
no side effects on ImGui. The recorder is small, pure, and fast: a
dozen field reads + a small `std::vector` append per frame.

### Shutdown

`EditorShell::~EditorShell` calls `reproRecorder_->flush()`. The
recorder writes to `<path>.tmp`, then renames atomically over
`<path>`. A `[repro] wrote N frames to <path>` line lands on stderr
on success; `[repro] flush failed — recording lost` with specifics
on failure.

### Memory footprint

~140 bytes per frame with no discrete events. A 10-minute session at
60 fps produces ~36k frames ≈ 5 MB in RAM, ~3–5 MB NDJSON on disk.
Not streamed to disk during recording (would complicate the atomic
write); flushed all at once on shutdown.

### Files

- `donner/editor/repro/ReproFile.{h,cc}` — data model + NDJSON I/O
- `donner/editor/repro/ReproRecorder.{h,cc}` — live capture
- `donner/editor/repro/ReproFile_tests.cc` — round-trip + rejection tests
- `donner/editor/repro/BUILD.bazel`
- `donner/editor/repro/README.md` — user docs
- `donner/editor/EditorShell.{h,cc}` — recorder owner + snapshotFrame hook
- `donner/editor/main.cc` — `--save-repro` CLI flag

## Stage 2 — Headless Replay (scoped, not built)

### Approach

Stand up a minimal driver that:

1. Loads a `.donner-repro` via `ReadReproFile`.
2. Creates an `EditorShell` driven by a **mock `EditorWindow`** — a
   subclass that never opens a GLFW window but still sets up an ImGui
   context + null display backend.
3. For each recorded frame:
   a. Set `ImGuiIO::DisplaySize` = recorded `wnd`/resize events.
   b. Call `io.AddMousePosEvent(mx, my)`.
   c. For each discrete event: `AddMouseButtonEvent`, `AddKeyEvent`,
      `AddInputCharacter`, `AddMouseWheelEvent`, etc.
   d. Call `ImGui::NewFrame`, `shell.runFrame`, `ImGui::EndFrame`.
   e. Tick the async renderer: `pollResult()`, wait for busy → idle,
      etc. At configurable checkpoint frames, call
      `AsyncRenderer::pollResult()` and cache the bitmap.
4. At the end (or at a configurable frame), take the render-pane
   bitmap and write it to a temp PNG.
5. `pixelmatch` against a committed golden with the same tolerance
   parameters as `ImageComparisonTestFixture`.

### Open questions

- **GLFW vs pure null backend.** `ImGui_ImplGlfw_InitForOpenGL`
  requires a valid `GLFWwindow`. Can we create one in "hidden"
  mode (`glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE)`) to satisfy
  the ImGui backend while not putting anything on screen? Or do
  we need a pure-null ImGui backend that skips `ImGui_ImplGlfw`
  entirely and injects IO directly? The former is simpler; the
  latter is more honest for CI environments with no display
  server at all. Prototype both before committing.
- **Async renderer determinism.** The worker thread runs on
  wall-clock time; a replay frame's render may or may not land in
  `AsyncRenderer::pollResult()` on the same frame it did during
  recording. Likely need to spin-wait after each replayed frame
  until `asyncRenderer.isBusy() == false` before advancing — turning
  real-time async into pseudo-synchronous playback. Wall-clock time
  in the recording is informational, not load-bearing for replay.
- **GL vs software bitmap for the render pane.** The editor
  normally uploads the render-pane bitmap to a GL texture and blits
  via ImGui; a null-GL environment can't do that. Short-circuit the
  render path in the mock window to just hold the `RendererBitmap`
  directly — the render pane contents are what we're asserting on,
  not the final GL framebuffer.
- **Golden image update flow.** Mirror the SVG renderer pattern:
  `UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run
  //donner/editor/repro/tests:repro_playback_tests` regenerates all
  goldens. The same env var hook that `ImageComparisonTestFixture`
  uses.

### Proposed test harness

```cpp
TEST_F(ReproPlaybackTest, CloudsCls8ClipArtifact) {
  auto repro = repro::ReadReproFile("testdata/repros/clouds_cls8_clip.donner-repro");
  ASSERT_TRUE(repro.has_value());
  ReproPlayer player;
  player.loadSvgFrom(repro->metadata.svgPath);
  for (const auto& frame : repro->frames) {
    player.advance(frame);
  }
  const RendererBitmap final = player.renderPaneBitmap();
  ASSERT_IMAGES_MATCH_GOLDEN(final, "testdata/golden/clouds_cls8_clip.png");
}
```

Each `.donner-repro` lives alongside its golden PNG in a
`testdata/repros/` directory. A regression from a real user bug
becomes: record, commit the `.donner-repro` and the golden PNG,
land a fix, and the test locks it in.

## Reversibility

Removing the feature: delete `donner/editor/repro/`, revert
`EditorShell.{h,cc}` and `main.cc` changes (each adds ≤20 lines). No
downstream code depends on the recorder; it's a leaf.

Rolling back an incorrect recording: delete the `.donner-repro` file.
It's non-load-bearing — the editor runs identically without it.

## Alternatives Considered

### Record at the `SelectTool` level

Simpler: wrap `SelectTool::onMouseDown/Move/Up` with a logging
decorator, store the call stream, replay by calling the same methods
in sequence.

Rejected because it only captures tool-dispatched events. Menu bar
clicks, keyboard shortcuts, sidebar interactions, text editor input,
and all widget-level ImGui state changes go through different
dispatch paths and would be invisible. A user's reported bug could
arise from any of them; tool-level replay would miss it. The "full
coverage by recording at the input boundary" argument wins decisively
against the "simpler implementation" counterargument.

### Record GLFW events directly

Even lower than ImGui — capture glfw's own event callbacks. Rejected
because:

1. GLFW events have a known unmarshaling path through
   `ImGui_ImplGlfw_*_Callback`s — replaying them means duplicating
   that plumbing. Capturing the ImGui IO output of that plumbing
   instead is the same correctness story with less code.
2. GLFW backend variance (Cocoa on macOS vs X11 vs Wayland on
   Linux) means the same user gesture emits different GLFW events on
   different platforms. Recording at ImGui input normalizes across
   platforms — the recording captures "the user moved the mouse to
   (523, 412) and pressed button 0", not the platform-specific
   callback sequence.

### Record rendering commands instead

Already exists via `SerializingRenderer` + `.rnr` files (see
`donner/editor/sandbox/`). Solves a different problem: reproducing
a rendering regression given a known scene + drawcall sequence. Can't
reproduce a compositor bug or a DOM-mutation bug because both happen
*above* the renderer. The two mechanisms are complementary.

### Use a real JSON library

nlohmann/json is already in the repo's dep tree (via CSS tests).
Rejected for the recorder side because it's a non-trivial addition
to the editor binary's link closure. A hand-rolled NDJSON writer
is ~80 LOC and has no dependency surface. The parser side is
similarly tiny and uses `std::strtod` for the only non-trivial
parse step. Stage 2's test harness may pull in nlohmann for richer
validation if needed; that's fine because tests already ship with
heavier deps.

## Open Questions

- **Should recording be always-on, with a silent in-memory rolling
  buffer?** A "save the last 5 minutes" hotkey would let users
  capture a bug they've already hit without needing to reproduce it
  with `--save-repro` on. Weigh against the ~5 MB resident cost.
  Defer.
- **Recording mid-session start.** Currently requires the `--save-repro`
  flag at launch. A runtime "start/stop recording" menu item would
  be convenient for long editing sessions where a bug shows up
  hours in. Low-priority.
- **Deterministic wall-clock timestamps during replay.** Do we set
  `ImGuiIO::DeltaTime` to the recorded `dt`, or to real wall-clock
  delta during replay? The former makes playback deterministic;
  the latter makes animation-driven content render faithfully.
  Pick the former; replays are for reproducibility, not smooth
  animation.

## Future Work

- **Stage 2: headless player + golden PNG harness.** Most of the
  above.
- **Visual diff inspector.** Given a failing replay, side-by-side
  render of actual vs golden plus a diff heatmap. Borrow from the
  renderer tests' `TerminalImageViewer`.
- **Repro minimization.** Given a failing `.donner-repro`, an
  automated tool that removes events one at a time and re-plays,
  producing the smallest event sequence that still reproduces.
  Hypothesis-testing bisection — straightforward once Stage 2 is
  solid.
- **Cross-platform normalization.** Document and test that a
  `.donner-repro` recorded on macOS replays identically on Linux
  and the emscripten build. Likely requires recording the
  platform-specific adjustments (HiDPI, scroll direction) in the
  metadata so the player can normalize.
