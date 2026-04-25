# Donner editor UI-input recording + replay

A `.donner-repro` file is a recording of the ImGui input state captured
at the top of every editor frame. Because the recording happens at the
raw input boundary — below menu action dispatch, below tool dispatch,
below the compositor — a replay exercises every stage of the stack.
Whether the bug lives in the renderer, the DOM, the RIC, the event
bridge, or somewhere in between, a recording made in the live editor
reproduces it.

## Contents

- [Recording a session](#recording-a-session)
- [Inspecting a recording](#inspecting-a-recording)
- [Editing a recording by hand](#editing-a-recording-by-hand)
- [Sharing with the maintainer as a bug report](#sharing-with-the-maintainer-as-a-bug-report)
- [File format reference](#file-format-reference)
- [What is and isn't recorded](#what-is-and-isnt-recorded)
- [Replay (not yet implemented)](#replay-not-yet-implemented)
- [Design rationale](#design-rationale)
- [Troubleshooting](#troubleshooting)

## Recording a session

Pass `--save-repro <path>` when launching the editor:

```bash
bazel run //donner/editor:editor -- \
    --save-repro /tmp/my_bug.donner-repro \
    path/to/input.svg
```

Do whatever produces the bug: click, drag, scroll, type, toggle menu
items, resize the window. Every ImGui frame captures the full input
state plus any discrete events that fired during that frame.

Close the editor (window X button or `Cmd+Q`) to flush the recording
to disk. You'll see on stderr:

```
[repro] wrote 847 frames to /tmp/my_bug.donner-repro
```

If the editor crashes before flush, the recording is lost — the
in-memory buffer isn't streamed to disk during recording. (Reason:
streaming would complicate the atomic-rename pattern we use for
crash-safe writes. If this becomes a problem in practice, we'll
revisit.)

### Tips for a clean recording

- **Start narrow.** Open just the SVG that reproduces the bug, not a
  broader test set. Less noise in the frame stream.
- **Warm up the compositor first.** Mouse over the render pane for a
  second before triggering the bug. The first frame or two involve
  startup rendering that's sometimes different from steady state.
- **Capture just the bug.** Long recordings are fine (~5 MB per 10
  min at 60 fps) but harder to reason about. If the bug is a
  single click-drag-release, record a 5-second session, not a
  5-minute one.
- **Don't resize the window mid-recording.** Resize events are
  recorded faithfully, but playback of a resize event triggers a lot
  of downstream invalidation; keep the window a fixed size if you
  can.

## Inspecting a recording

`.donner-repro` is NDJSON — one JSON object per line. `head`, `cat`,
`jq`, and your text editor all work:

```bash
head -1 /tmp/my_bug.donner-repro | jq
```

```json
{
  "v": 2,
  "svg": "donner_splash.svg",
  "wnd": [1600, 900],
  "scale": 2.0,
  "exp": 0,
  "at": "2026-04-19T12:34:56Z"
}
```

```bash
# Show just frames where mouse button state changed
jq 'select(.e) | .e[] | select(.k == "mdown" or .k == "mup")' \
    /tmp/my_bug.donner-repro
```

```bash
# Count events by kind
jq -r 'select(.e) | .e[] | .k' /tmp/my_bug.donner-repro | sort | uniq -c
```

```bash
# Find the frame where a drag started
jq 'select(.e) | select(.e[].k == "mdown")' /tmp/my_bug.donner-repro
```

## Editing a recording by hand

Because the format is line-oriented, you can trim or annotate a
recording in your favorite editor.

- **Trim the prelude:** delete all frame lines up to the one just
  before the bug-triggering interaction. Keep the metadata line.
- **Trim the postscript:** delete all frame lines after the bug
  shows.
- **Re-number frame indices** if you care about gap-free `f` values
  (replay doesn't — `f` is informational):

```bash
awk 'NR==1{print;next} {gsub(/"f":[0-9]+/, "\"f\":" (NR-2)); print}' \
    in.donner-repro > out.donner-repro
```

Line-level edits are safe: each line parses independently. If you
break the JSON on one line, the loader rejects the whole file with
a specific error.

## Sharing with the maintainer as a bug report

A `.donner-repro` + a description of what you expected is a high-
fidelity bug report that's easier to act on than a screenshot.
Suggested template:

```
SVG: donner_splash.svg
Editor args: --experimental (if applicable)
Window size at start: 1784x1024
OS/platform: macOS 14.2 (ARM64)
Bug: dragging #Clouds_with_gradients leaves a crescent-shaped
     color drift at the top of the cls-8 cloud orb.
Expected: orb moves cleanly with its parent group, no drift.
Artifact visible from: frame ~120 onward (about 2 seconds in).
Recording: attached `cls8_clip.donner-repro` (847 frames, ~14 sec).
```

With the `.donner-repro` in hand, the maintainer can:
- Inspect the event sequence to understand what you did.
- (Once Stage 2 lands) replay it through the headless player for a
  reliable bisect target.
- Extract a synthetic test that mirrors the relevant events.

## File format reference

See `ReproFile.h` for the data model. Compact summary:

```
Line 1: {"v":2,"svg":"path.svg","wnd":[W,H],"scale":S,"exp":0|1,"at":"ISO8601"}
Line N: {"f":N,"t":seconds,"dt":milliseconds,"mx":X,"my":Y,
         "btn":BUTTON_MASK,"mod":MODIFIER_MASK,
         "mdx":DOC_X,"mdy":DOC_Y,
         "vp":{"ox":PANE_ORIGIN_X,"oy":PANE_ORIGIN_Y,"pw":PANE_W,"ph":PANE_H,
               "dpr":DPR,"z":ZOOM,
               "pdx":PAN_DOC_X,"pdy":PAN_DOC_Y,
               "psx":PAN_SCREEN_X,"psy":PAN_SCREEN_Y,
               "vbx":VB_X,"vby":VB_Y,"vbw":VB_W,"vbh":VB_H},
         "e":[Event, Event, ...]}
```

**Button mask bits:** `1<<0`=left, `1<<1`=right, `1<<2`=middle.

**Modifier mask bits:** `1<<0`=Ctrl, `1<<1`=Shift, `1<<2`=Alt, `1<<3`=Super.

**Event kinds** (tag in `k` field):

| Kind | Fields | Notes |
|------|--------|-------|
| `mdown` | `b`, `hit` (see below) | Edge of a bit in the frame's button mask |
| `mup`   | `b`                    | Edge of a bit in the frame's button mask |
| `kdown` / `kup` | `key` (ImGuiKey int), `m` (modifier mask) | See the watchlist in `ReproRecorder.cc` |
| `chr` | `c` (UTF-32 code point) | From `io.InputQueueCharacters` |
| `wheel` | `dx`, `dy` (float) | Per-frame wheel delta when non-zero |
| `resize` | `w`, `h` (int) | `ImGuiIO::DisplaySize` changed |
| `focus` | `on` (0/1) | Window focus gained/lost |

The continuous signal (`mx, my, btn, mod`) is captured every frame;
discrete events are emitted only on transitions. Replay logic can
rely on the continuous signal as the source of truth and treat
discrete events as hints — the next frame's state wins regardless.

**`mdx` / `mdy` (v2):** mouse position in SVG-document coordinates,
computed by the live editor from the current viewport. Replay should
use these directly rather than reconstructing screen→doc math from
pane-layout heuristics. Absent when the cursor was outside the render
pane (e.g. hovering the source pane / sidebar), in which case
document-space dispatch is a no-op.

**`vp` block (v2):** full viewport snapshot — pane origin/size, zoom,
pan anchor, viewBox, DPR. Delta-encoded: emitted only when different
from the prior frame's snapshot. Readers carry the prior block
forward so every frame sees a populated viewport.

**`mdown.hit` (v2):** the element the editor hit-tested at mouse-down
time. Schema: `{"tag":"g","id":"big_lightning_glow","idx":42}`, or
`{"empty":1}` for a click on empty space. **Diagnostic only — not a
load-bearing replay checkpoint.** Selection semantics downstream of
hit-test (e.g. filter-group elevation) can legitimately change across
code versions without invalidating the recording; use this field for
human-readable "which element got clicked" reports, not for test
assertions that would force a rerecord on every internals refactor.

### v1 → v2

v1 recordings had no viewport, no doc coords, and no hit checkpoint.
The replayer had to reconstruct screen→doc math from hand-tuned
pane-layout constants, which silently routed clicks to the wrong
elements whenever the live ImGui layout drifted. The loader rejects
v1 files with a diagnostic asking the user to rerecord.

## What is and isn't recorded

**IS recorded:**
- Mouse position + button state, every frame
- Mouse position in SVG-document space (v2; when cursor is inside the
  render pane)
- Render-pane viewport state — pane origin/size, zoom, pan, viewBox,
  DPR — delta-encoded (v2)
- Hit-test result at left-mouse-down (v2; tag, id, doc-order index)
- Mouse wheel deltas
- Keyboard key transitions (for the curated watchlist — letters,
  digits, function keys, modifiers, nav, arrow keys, common symbols)
- Character input (everything typed into an `InputText` widget)
- Window resize / focus
- Starting SVG path, window size, display scale, `--experimental`
  flag

**IS NOT recorded:**
- The actual rendered output. A `.donner-repro` is a script of your
  inputs, not a video of the editor's output. You need the golden
  PNG (Stage 2) to know what "correct output" should be.
- File I/O. If your bug involves loading a different SVG mid-session
  (File → Open), the new file path is recorded indirectly via the
  dialog clicks, but the file contents aren't. Replay has to
  resolve the same path on its end.
- Clipboard state. Copy/paste operations are captured as key
  events, but the clipboard contents aren't.
- System-level state (monitor DPI at record time, OS theme, fonts,
  GL driver version).
- Timing. `dt` is recorded but replay deliberately ignores wall-
  clock time — playback is frame-stepped, deterministic.
- Tracy / profiler state. Use Tracy separately if you need perf
  context.

## Replay (not yet implemented)

Stage 2 of this design. See
`docs/design_docs/0029-ui_input_repro.md` for the architecture and
open questions. Summary: a headless driver that stands up an ImGui
context + a mock `EditorWindow`, injects the recorded input events
frame-by-frame, and compares the final render-pane bitmap against a
committed golden PNG.

Until Stage 2 lands, recordings are still useful as high-fidelity
bug reports. Share the `.donner-repro` file with the maintainer;
they can read the event stream, understand your exact sequence, and
either build a synthetic test from it or run it through the player
once it's built.

## Design rationale

**Why record raw ImGui input, not higher-level events?** Because you
don't know where the bug lives. Recording at
`SelectTool::onMouseDown` misses menu clicks, keyboard shortcuts,
sidebar interactions, text editor input, pinch gestures, and
everything ImGui handles before dispatch reaches tools. Recording
at the GLFW layer is even better in principle but requires
platform-specific marshalling at replay. ImGui input is the
sweet spot: below all dispatch, above platform variance.

**Why NDJSON instead of a binary format?** Human-readable.
Hand-editable. Diffable. Line-oriented so you can trim one
interaction out of a long session with `sed`. The overhead vs
binary is ~3-5x in file size, which matters zero for a tool that
produces 5 MB files.

**Why flush on shutdown instead of streaming to disk?** The
atomic-rename pattern that protects against crash-mid-write doesn't
compose cleanly with streaming. We could add an "append mode" later
if someone records a bug that crashes the editor before flush; for
now, crashes-before-flush lose the recording and that's understood.

See the design doc (`docs/design_docs/0029-ui_input_repro.md`) for
the full set of alternatives considered and open questions.

## Troubleshooting

**`--save-repro` flag rejected.** You need a filename argument
immediately after the flag: `--save-repro /tmp/foo.donner-repro`,
not `--save-repro` alone.

**No `[repro] wrote N frames …` message on exit.** Check that you
actually passed the flag and that the editor didn't crash. If it
crashed, the buffer wasn't flushed.

**Loader rejects the file.** Check the stderr output — it prints
the specific line and error. Common causes: file truncation
(flush didn't complete), version mismatch (file from an older
format), hand-edit broke a line's JSON syntax.

**Frame count is huge for a short session.** Expected: the recorder
snapshots every frame the editor renders, which includes
idle-animation-repaint frames. 60 fps × 10 sec = 600 frames even if
you just moved the mouse once.

**Recording file is empty.** The metadata-only case (recording
started but zero frames captured) can happen if you close the
editor before it draws one frame. Try recording again and ensure at
least one render happens before close.
