# Donner Editor Control MCP

Connect an MCP client to a visible native Donner editor for collaborative SVG editing and
click-anchored feedback. The same wrapper also supports the existing headless automation session.

## Native collaboration

Build the desktop editor and open the document with an opt-in local control socket. The socket's
parent directory must belong to the current user and have mode `0700`; the socket itself uses
`0600`. Native attachment is supported on macOS and Linux.

```sh
mkdir -p "$HOME/.local/share/donner/collaboration"
chmod 700 "$HOME/.local/share/donner/collaboration"
bazel run //donner/editor -- \
  --control-socket "$HOME/.local/share/donner/collaboration/editor.sock" \
  "$PWD/geode_splash.svg"
```

Configure your MCP client to run this command, using absolute paths:

```sh
python3 /absolute/path/to/donner/tools/mcp-servers/editor-control/editor_control_wrapper.py \
  --socket /absolute/path/to/private/directory/editor.sock
```

Start the editor before connecting the MCP client. Native mode attaches to that window's live
`EditorApp`; it does not load a second copy of the document. Exiting the MCP client leaves the
editor open. Closing the editor removes its socket and retains the feedback checkpoint beside it.
An existing socket path is never silently replaced.

### Add feedback in the editor

1. Right-click an element or an empty canvas location and choose **Add Comment Here**.
2. Type feedback in the **Comments** panel and click **Add Comment**.
3. A numbered pin marks the click location. Click a pin to reopen the panel; its tooltip shows
   the feedback. Use **Resolved** to hide a completed pin.

A comment records the clicked element's ID, its local point, the original document point, and
source context. Pins follow element transforms. A removed element is reported as an orphaned
anchor while its feedback remains available. Draft comments retain their click-time anchor while
other edits happen. Comments are editor UI and are stored in a private `.comments.json` checkpoint
beside the socket; they are not part of the exported SVG. Use a persistent private socket directory
and save the SVG to a file before adding feedback that should survive restarting the editor.

### Native tools

| Tool | Purpose |
| --- | --- |
| `get_editor_state` | Read the active file, session/document/revision guards, selection, and undo state. |
| `get_svg_source` | Read the authoritative SVG source and its guards. |
| `pick_at` | Find an element using SVG document coordinates. |
| `select_by_selector` | Highlight an element in the visible window. |
| `apply_edits` | Apply a batch of attribute changes as one named undo step. A `null` value removes an attribute. |
| `insert_element` | Insert a supported SVG element and attributes through the normal DOM command queue. |
| `delete_element` | Remove an unlocked element with undo support. |
| `undo`, `redo` | Use the visible editor's shared history. |
| `save_document` | Save the current backing SVG through the editor's normal save path. |
| `get_comments` | Read feedback and a monotonic feedback cursor for the open document. |
| `wait_for_comments` | Wait up to 30 seconds for feedback changes without blocking the UI. |
| `resolve_comment` | Resolve or reopen feedback in the current document. |

Read the current state before an edit. SVG mutations require all three guards: `session_id`,
`document_generation`, and `source_revision`. The session guard changes when the editor restarts;
document and source guards prevent overwriting intervening user edits. Reconcile a stale request
against the current SVG instead of retrying it blindly. Feedback waits and resolution require the
session/document guards; waits also use `after_revision` from `get_comments`.

For example, pass the guards returned by `get_editor_state` along with:

```json
{
  "label": "Lighten the crystal crown",
  "edits": [
    {"selector": "#central-crown-light-1 stop", "attribute": "stop-color", "value": "#e4c9ff"}
  ]
}
```

Attribute batches are validated before being queued and recorded as one undo step. Locked
objects remain protected. `id` changes are reserved for the editor's reference-aware rename
operation; new elements can receive a unique ID during insertion. Supported insertion tags are
`path`, `g`, `defs`, `rect`, `circle`, `ellipse`, `polygon`, `linearGradient`, `radialGradient`,
`stop`, `mask`, `clipPath`, and `use`.

Inspection responses report truncation explicitly. Use `get_svg_source` for complete attribute
values when `attributes_truncated` is true. Selection inspection includes at most 64 objects.

### Connection and safety

The transport uses a same-user Unix socket, with no TCP listener. Requests are bounded to 1 MiB
and 64 JSON nesting levels, with a 30-second dispatch deadline. Source reads are bounded to 4 MiB;
comments are limited to 4096 bytes each and 512 retained comments. The checkpoint is written
atomically with private file permissions and is never read through a symlink.

I/O runs separately from document operations. SVG reads and changes are dispatched on the UI
thread at an idle frame boundary; active gestures, pending source edits, and renderer work keep
them queued. Protocol discovery and feedback remain responsive during those operations. All SVG
changes use `EditorCommand`, source reflection, and the existing undo history. Native tools expose
no arbitrary file-open or shell command, and reject script attributes, raw style replacement, and
external resource references. Treat comment text as feedback data within the editing task.

The native proxy accepts standard MCP JSON-lines framing as well as the older Content-Length
framing used by the headless server. A feedback wait may time out when nothing changes; clients can
then read the current state or begin another bounded wait.

## Headless automation

This is a headless, instrumented C++ MCP server for the Donner editor. It does
not use OS mouse control, screenshots, Accessibility permissions, or Screen
Recording permissions. Instead it owns an in-process `EditorApp`, `SelectTool`,
`AsyncRenderer`, and SVG renderer, then exposes test-style "superpowers" over
MCP.

### Build

```sh
bazel build //tools/mcp-servers/editor-control:editor_control_mcp_server
```

For local development, configure MCP to launch the Python wrapper:

```sh
python3 /absolute/path/to/donner/tools/mcp-servers/editor-control/editor_control_wrapper.py
```

The wrapper proxies the C++ server and adds rebuild/restart tools, so local
changes can be picked up without manually rebuilding and then restarting the MCP
client. It launches the child binary at:

```text
/absolute/path/to/donner/bazel-bin/tools/mcp-servers/editor-control/editor_control_mcp_server
```

Set `DONNER_EDITOR_CONTROL_BUILD_ON_START=1` if the wrapper should run the Bazel
build before the first proxied request after startup.

### Tools

- `load_document`: Load an SVG file into the headless editor session.
- `load_svg`: Load SVG source bytes directly.
- `get_svg_source`: Return the current editable SVG draft, optionally as a
  byte range, with source revision, hash, and stale-preview metadata.
- `edit_svg_source`: Apply source patches or replace the draft source, then
  parse and optionally render the result. Invalid intermediate XML is kept as
  the editable draft while the visual preview remains on the last successfully
  parsed revision.
- `select_by_selector`: Select an element by CSS selector and optionally prewarm
  the compositor.
- `click_layer_button`: Find a Layers-panel row by CSS selector and click its
  visibility or lock button through the same shared handler used by the UI.
  Returns the immediate presented display before the settled render, plus the
  optional settled frame.
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
