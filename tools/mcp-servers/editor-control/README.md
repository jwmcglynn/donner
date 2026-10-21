# Donner Editor Control MCP

This is a headless, instrumented C++ MCP server for the Donner editor. It does
not use OS mouse control, screenshots, Accessibility permissions, or Screen
Recording permissions. Instead it owns an in-process `EditorApp`, `SelectTool`,
`AsyncRenderer`, and SVG renderer, then exposes test-style "superpowers" over
MCP.

## Build

```sh
bazel build //tools/mcp-servers/editor-control:editor_control_mcp_server
```

For local development, configure MCP to launch the Python wrapper:

```sh
python3 /path/to/donner/tools/mcp-servers/editor-control/editor_control_wrapper.py
```

The wrapper proxies the C++ server and adds rebuild/restart tools, so local
changes can be picked up without manually rebuilding and then restarting the MCP
client. It launches the child binary at:

```text
/path/to/donner/bazel-bin/tools/mcp-servers/editor-control/editor_control_mcp_server
```

Set `DONNER_EDITOR_CONTROL_BUILD_ON_START=1` if the wrapper should run the Bazel
build before the first proxied request after startup.

## Tools

- `load_document`: Load an SVG file into the headless editor session.
- `load_svg`: Load SVG source bytes directly.
- `get_svg_source`: Return the current editable SVG draft, optionally as a
  byte range, with source revision, hash, and stale-preview metadata.
- `edit_svg_source`: Apply source patches or replace the draft source, then
  parse and optionally render the result. Invalid intermediate XML is kept as
  the editable draft while the visual preview remains on the last successfully
  parsed revision.
- `select_by_selector`: Select the first element matching a CSS selector, in
  document order starting with the root `<svg>` (so `:root` selects the root),
  and optionally prewarm the compositor. Every selector-based tool resolves
  selectors this way.
- `pointer_gesture`: Dispatch a click, double-click, or drag through the active
  canvas tool. A plain select-tool press first tries the editor shell's
  selected-bounds re-drag (inside the single selected element's bounds, off its
  transform handles and outside later-painted bounds) before hit testing, as
  the editor does. The shell's drag-intent retry for a press held over
  later-painted bounds is not modeled.
- `click_layer_button`: Find a Layers-panel row by CSS selector and click its
  visibility or lock button through the same shared handler used by the UI.
  Returns the immediate presented display before the settled render, plus the
  optional settled frame.
- `history`: Undo or redo one entry of the editor's undo timeline through the
  same `EditorApp` path as Edit > Undo / Redo. Returns whether an entry was
  applied, the undo label before and after, and the updated source revision,
  so a client can check which edit a Cmd+Z actually reverses.
- `drag_selector`: Find an element by CSS selector, synthesize click/drag frames
  through `SelectTool`, and return per-frame compositor and presentation tile
  metadata.
- `transform_selector`: Find an element by CSS selector, synthesize a scale
  corner-handle or rotate-ring drag through `SelectTool`, and return per-frame
  compositor and presentation tile metadata. Use `mode: "scale"` or
  `mode: "rotate"` with a `corner` such as `bottom_right`.
- `render_frame`: Render the current editor state and return the final frame
  plus split compositor and presentation tile metadata.
- `session_state`: Inspect selection, canvas, and compositor diagnostic state.
- `inspector_state`: Refresh the Inspector panel's model for the current
  selection and return its title, the document-space bounds behind the Position
  and Size fields (`null` when the Inspector has none), and the transform.
- `start_rnr_recording`: Start recording subsequent MCP-driven gestures to the
  existing editor `.rnr` NDJSON format.
- `stop_rnr_recording`: Stop the active recording and optionally write it to
  disk.
- `rnr_recording_state`: Inspect the active recording.
- `replay_rnr`: Load an `.rnr`, resolve its SVG, replay mouse events through
  `SelectTool`, and optionally return per-render compositor/presentation
  metadata. Pass `gl_readback: true` with `gl_capture_frame` or
  `gl_capture_left_mousedown` to replay through the real OpenGL editor shell and
  return framebuffer PNGs; `gl_drive_document_input: true` uses recorded
  document-space mouse coordinates for MCP-generated replays,
  `gl_source_pane_visible: true` sets the source pane's animation target to visible before the
  first replay frame,
  so frame zero captures the beginning of its slide-in transition, and
  `gl_crop: "document-canvas"` hides source and side panels in the capture. Raw
  `bazel-bin` MCP launches route GL readback through `bazel run //donner/editor/tests:editor_rnr_gl_replay` so macOS Cocoa/GL initialization
  happens in the same environment as the replay helper.
- `editor_control_wrapper_state`: Inspect the Python wrapper, child process, and
  last build result.
- `restart_editor_control_server`: Restart the child C++ MCP server, optionally
  rebuilding first.
- `rebuild_editor_control_server`: Run the Bazel build for the child C++ MCP
  server and restart it by default after a successful build.

`render_frame`, `drag_selector`, and `transform_selector` can attach the final
frame as PNG MCP image content. Tile PNGs are opt-in because the split layer
list can be large on the splash SVG.

Source editing is revision-guarded: pass `expected_source_revision` to
`edit_svg_source` after a `get_svg_source` call to avoid applying offsets to a
draft that changed underneath the MCP client. `render_frame`, `session_state`,
and source-edit responses include `preview_stale` so an agent can tell when the
attached image is the last valid rendering rather than the current draft.

A completed gesture's source writeback is applied before any later tool reads,
reports or records source state: `get_svg_source`, `session_state`, the
`pointer_gesture` response and the source embedded by `start_rnr_recording`
include the last drag or transform, and their `source_revision` matches what a
following `render_frame` or `edit_svg_source` call observes, so an
`expected_source_revision` read after a gesture is accepted. Because it applies
the writeback, `session_state` also reports the dirty state that writeback
produced.

Each render stage includes both:

- `composited_preview`: the worker-side split tile list from `AsyncRenderer`.
- `display_preview`: the headless presentation view after the editor-side tile
  cache decides which composited tiles the UI would blit.

Drag frames also include `display_before_render`, which captures the
presentation state immediately after the synthetic input event and before the
next async render result lands. This is the frame that catches stale cached-tile
handoff bugs during drag-target switches.
Transform previews include both `translation_doc` and
`document_from_cached_document`, plus each presentation tile's
`effective_document_from_cached_document`, so MCP clients can validate affine
scale/rotate handoffs instead of only translation drags.
When `include_display_diff` is enabled, `differing_pixels` is the
pixelmatch mismatch count. Any emitted `diff_*` PNG is the pixelmatch visual
diff for the same comparison.

The headless recorder writes v2 `.rnr` frames with document-space coordinates
and viewport snapshots. It records MCP-synthesized gestures, not OS-level mouse
input; live GUI recording remains owned by the editor's `--save-repro` path.

To reproduce a render-submission race, `replay_rnr` accepts
`gl_worker_document_hold_start_frame` and `gl_worker_document_hold_end_frame`.
The first frame pauses queued workers before document acquisition; the second
releases the pause and waits for the submitted render before replay continues.
Both frames must exist within the replay range. The control is replay-only and
shutdown always releases it. The replay CLI exposes the equivalent
`--worker-document-hold-start-frame` and `--worker-document-hold-end-frame` options.
