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
python3 /Users/jwm/Projects/donner/tools/mcp-servers/editor-control/editor_control_wrapper.py
```

The wrapper proxies the C++ server and adds rebuild/restart tools, so local
changes can be picked up without manually rebuilding and then restarting the MCP
client. It launches the child binary at:

```text
/Users/jwm/Projects/donner/bazel-bin/tools/mcp-servers/editor-control/editor_control_mcp_server
```

Set `DONNER_EDITOR_CONTROL_BUILD_ON_START=1` if the wrapper should run the Bazel
build before the first proxied request after startup.

## Tools

- `load_document`: Load an SVG file into the headless editor session.
- `load_svg`: Load SVG source bytes directly.
- `select_by_selector`: Select an element by CSS selector and optionally prewarm
  the compositor.
- `drag_selector`: Find an element by CSS selector, synthesize click/drag frames
  through `SelectTool`, and return per-frame compositor and presentation tile
  metadata.
- `render_frame`: Render the current editor state and return the final flat frame
  plus split compositor and presentation tile metadata.
- `session_state`: Inspect selection, canvas, and compositor diagnostic state.
- `start_rnr_recording`: Start recording subsequent MCP-driven gestures to the
  existing editor `.rnr` NDJSON format.
- `stop_rnr_recording`: Stop the active recording and optionally write it to
  disk.
- `rnr_recording_state`: Inspect the active recording.
- `replay_rnr`: Load an `.rnr`, resolve its SVG, replay mouse events through
  `SelectTool`, and optionally return per-render compositor/presentation
  metadata. Pass `gl_readback: true` with `gl_capture_frame` or
  `gl_capture_left_mousedown` to replay through the real OpenGL editor shell and
  return framebuffer PNGs; `gl_crop: "document-canvas"` hides source and side
  panels in the capture.
- `editor_control_wrapper_state`: Inspect the Python wrapper, child process, and
  last build result.
- `restart_editor_control_server`: Restart the child C++ MCP server, optionally
  rebuilding first.
- `rebuild_editor_control_server`: Run the Bazel build for the child C++ MCP
  server and restart it by default after a successful build.

`render_frame` and `drag_selector` can attach the final frame as PNG MCP image
content. Tile PNGs are opt-in because the split layer list can be large on the
splash SVG.

Each render stage includes both:

- `composited_preview`: the worker-side split tile list from `AsyncRenderer`.
- `display_preview`: the headless presentation view after the editor-side tile
  cache and drag-presentation gates decide whether the UI would blit tiles or
  fall back to the flat frame.

Drag frames also include `display_before_render`, which captures the
presentation state immediately after the synthetic input event and before the
next async render result lands. This is the frame that catches stale cached-tile
handoff bugs during drag-target switches.

The headless recorder writes v2 `.rnr` frames with document-space coordinates
and viewport snapshots. It records MCP-synthesized gestures, not OS-level mouse
input; live GUI recording remains owned by the editor's `--save-repro` path.
