---
name: donner-editor-debugging
description: >-
  Reproduce and root-cause Donner editor visual/interaction bugs via .rnr replay
  (editor_rnr_gl_replay), the editor-control MCP server, layer-by-layer escalation, and
  deterministic replay scheduling. Use when debugging one-frame glitches, overlay jumps, stale
  drag content, checkerboard flashes, wrong-scale textures, or element pop-backs; when an editor
  screenshot or .rnr replay is needed; or when driving or extending the editor-control MCP under
  tools/mcp-servers/editor-control/.
---

# Donner Editor Debugging

How to reproduce, screenshot, and root-cause editor bugs. Depth lives in
`docs/editor_visual_debugging.md` (layer table, failure signatures, known root causes) and
`docs/deterministic_replay_testing.md` (deterministic scheduling API); this skill is the
operational how-to.

## Non-negotiable rules

- **Repro before fix.** Write an automated repro that fails at HEAD before touching product code;
  a fix without a red-to-green transition is an `attempt:`, not a fix. See donner-bugfix-discipline.
- **MCP-first for editor QA bugs.** Order is: (1) make the editor-control MCP reproduce the
  user-visible failure (extend the MCP first if it lacks the gesture/capture surface), (2) write
  the narrowest regression test and prove it fails at HEAD, (3) fix to green, (4) rerun the MCP
  repro and record that evidence. A green unit test without MCP re-verification does not close a
  visual or interaction bug — the unit test may model the wrong thing.
- **Overlay lockstep.** Path overlays and selection chrome must use the same presented transform
  as the document pixels beneath them in the same frame. Same-older-transform for both beats
  fresh chrome over stale pixels. A frame violating this is a bug even if it settles next frame.
- **No broad cache clears.** Never fix a presentation bug with `resetComposited()`,
  `resetForLoadedDocument()`, full compositor resets, or forced reparses for incremental edits
  (drag, delete, attribute change). Broad clears cause one-frame checkerboard flashes and hide
  the real invalidation bug. Use targeted entity/tile/region invalidation (AGENTS.md).
- **Anti-aliasing is a banned root cause.** pixelmatch already filters AA edge pixels; any
  flagged diff is real. Large diffs (hundreds of pixels, whole-shape offsets) are never AA.
- **Replay screenshots, not OS screenshots, are the proof of record.** They are deterministic
  and capture the same frame a test can assert.

**Stale-doc warning:** older docs and design docs still name `EditorBackendCore` — that type no
longer exists in source. The live orchestration types are `EditorShell`
(donner/editor/EditorShell.h), `RenderCoordinator` (donner/editor/RenderCoordinator.h), and
`AsyncRenderer` (donner/editor/AsyncRenderer.h); `CompositorController` lives in
donner/svg/compositor/. Replay tests are `donner/editor/tests/RnrReplay_tests.cc` (non-GL) and
`GlRnrReplay_tests.cc` (GL). Verify any path or symbol a doc cites before relying on it.

## Choosing a tool

| Need                                                                                                                                        | Tool                                                                               |
| ------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| Frame-exact PNG capture, before/after pixel comparison                                                                                      | `editor_rnr_gl_replay` CLI (below)                                                 |
| Interactive repro building: select/drag/scale/edit source + per-frame tile metadata                                                         | editor-control MCP                                                                 |
| Record a new repro from the live GUI (needs a real display + mouse; in headless/agent sessions use the MCP's `start_rnr_recording` instead) | `bazel run --config=geode //donner/editor:editor -- --save-repro out.rnr file.svg` |
| Timing-sensitive assertion in a committed test                                                                                              | `RunGlRnrReplay` with deterministic scheduling                                     |
| Isolate a layer (viewport math, texture cache, ...)                                                                                         | unit tests listed under "Layer escalation"                                         |

Sample `.rnr` repros are checked in under `donner/editor/tests/` (e.g.
`filter_elm_disappear-7.rnr`, `geode_drag_zoom_o_pop.rnr`); `ls donner/editor/tests/*.rnr` lists
the current set. Before guessing frame numbers, `head -c 600` the `.rnr`: curated repros embed an
`expect` block in the first-line JSON header (`min_frame_index`/`max_frame_index`,
`target_selector`, `crop_mode`, sometimes a pixel `crop` — see `ReproExpectation` in
`donner/editor/repro/ReproFile.h`). The replay CLI does not act on it automatically; use it as
your first `--capture-frame` / `--crop` values.

On Linux/headless hosts, set up Mesa llvmpipe per donner-geode-backend before running any
`--config=geode` command below — the hardware GPU path can hang instead of failing. (macOS with a
real GPU needs no setup.)

## Replay CLI: editor_rnr_gl_replay

Source: `donner/editor/tests/EditorRnrGlReplay.cc` (arg parsing) over the shared harness
`donner/editor/repro/GlRnrReplay.{h,cc}`. Standard screenshot run (from repo root; `bazel run`
resolves relative paths via `BUILD_WORKING_DIRECTORY`):

```sh
bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- \
  --rnr /private/tmp/repro.rnr \
  --svg donner_splash.svg \
  --out-dir /private/tmp/editor-repro \
  --capture-frame 78 --capture-frame 79 --capture-frame 80 \
  --max-frame 81 \
  --crop document-canvas
```

At least one capture selector (`--capture-frame N`, repeatable, or `--capture-left-mousedown K`
= K-th left-button press) is required. Other flags: `--visible`, `--no-pace`,
`--drive-document-input` (use recorded document-space coords — needed for MCP-recorded `.rnr`),
`--content-only-capture`, `--worker-delay-ms N`, `--worker-scheduling realtime|drain-each-frame|hold-frames-behind`, `--hold-frames-behind N`, `--print-diagnostics`,
`--diagnostics-frame N`.

Output PNGs are named `gl_replay_frame_<N>_<reason>[_<crop>].png` where reason is `explicit` or
`left_mousedown_<K>` and crop suffix is `canvas` (document-canvas), `render_pane`, or empty
(full). Do not guess names — the tool prints JSON to stdout listing the absolute path of every
capture.

**Crop decision tree:**

- `document-canvas` — canvas/shape bugs (most cases).
- `full` — ImGui chrome, pane layout, dialogs, sidebars, layer thumbnails, framebuffer size.
- `render-pane` — bug inside the center viewport but dependent on pan/zoom chrome.

**Discipline:**

- Inspect the PNGs BEFORE adding `--print-diagnostics` or instrumentation — diagnostics change
  timing and can destroy a one-frame repro.
- For before/after comparisons keep `.rnr`, `--svg`, `--capture-frame`, `--crop`, and pacing
  flags identical, otherwise the diff measures your harness change, not the fix.
- For timing-sensitive failures run both paced (default) and `--no-pace`: unpaced is faster and
  more deterministic but can hide races that need realistic frame spacing.

## editor-control MCP

Headless in-process editor (no OS mouse/screenshot permissions). Sources:
`tools/mcp-servers/editor-control/` — README.md, `EditorControlSession.cc` (`toolList()` is the
authoritative tool + schema list; re-read it when in doubt, tools are added over time).

**Connecting to Claude Code:** the MCP tools are not available until the server is registered —
check first (e.g. try a tool lookup for `load_document`/`drag_selector`). If they aren't
callable, register and restart the session (repo root as cwd):

```sh
claude mcp add donner-editor-control -- python3 tools/mcp-servers/editor-control/editor_control_wrapper.py
```

`.vscode/mcp.json` registers it for VS Code only. If registration isn't possible in the current
session, fall back to the replay CLI above.

Build: `bazel build //tools/mcp-servers/editor-control:editor_control_mcp_server`.
Dev launch (proxies the C++ server, adds `rebuild_editor_control_server`,
`restart_editor_control_server`, `editor_control_wrapper_state`):
`python3 tools/mcp-servers/editor-control/editor_control_wrapper.py`. The wrapper resolves the
repo root from its own script location (the README's absolute example path is stale — ignore
it). Env overrides: `DONNER_EDITOR_CONTROL_BINARY`, `DONNER_EDITOR_CONTROL_REPO_ROOT`,
`DONNER_EDITOR_CONTROL_BUILD_ON_START=1` (build before first request).

Tools (arguments in parentheses; required bold):

| Tool                                                                 | Key arguments                                                                                                                                          | Purpose                                                      |
| -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------ |
| `load_document`                                                      | **svg_path**, canvas_width/height, device_pixel_ratio, render_after_load                                                                               | Load an SVG file                                             |
| `load_svg`                                                           | **svg_source**, source_path, canvas sizing as above                                                                                                    | Load SVG bytes directly                                      |
| `get_svg_source`                                                     | offset, length                                                                                                                                         | Read draft source + `source_revision`, hash, `preview_stale` |
| `edit_svg_source`                                                    | edits:[{**offset**, delete_count, insert}] or replace_source; expected_source_revision                                                                 | Revision-guarded incremental source edits                    |
| `select_by_selector`                                                 | **selector** (CSS), render                                                                                                                             | Select first matching element                                |
| `click_layer_button`                                                 | **selector**, button: "visibility"\|"lock"                                                                                                             | Layers-panel row buttons via the UI's handler                |
| `set_active_tool`                                                    | **tool**: "select"\|"pen"                                                                                                                              | Switch canvas tool (recorded into .rnr)                      |
| `set_style_property`                                                 | **property**, **value** (e.g. fill, #ff0000)                                                                                                           | Set paint on current selection                               |
| `pen_path`                                                           | **points**:[{x,y}], close, commit_open                                                                                                                 | Draw a path via PenTool clicks                               |
| `drag_selector`                                                      | **selector**, delta_x, delta_y, frames (max 240), selection_mode, release                                                                              | Synthesize click+drag through SelectTool                     |
| `transform_selector`                                                 | **selector**, mode: "scale"\|"resize"\|"rotate", corner (e.g. bottom_right), delta_x/y, shift, option                                                  | Corner-handle scale or rotate-ring drag                      |
| `render_frame`                                                       | include_final_frame, include_tile_images, embed_png_base64                                                                                             | Render + split compositor tile metadata                      |
| `session_state`                                                      | (none)                                                                                                                                                 | Selection, canvas, compositor diagnostics                    |
| `start_rnr_recording` / `stop_rnr_recording` / `rnr_recording_state` | output_path, svg_path, window sizing                                                                                                                   | Record MCP gestures to `.rnr`                                |
| `replay_rnr`                                                         | **rnr_path**, svg_path, gl_readback, gl_capture_frame, gl_capture_left_mousedown, gl_max_frame, gl_crop, gl_drive_document_input, include_display_diff | Replay an `.rnr` in-session or through real GL               |

Example — reproduce a drag glitch and read the per-frame handoff state:

```json
{"name": "load_document", "arguments": {"svg_path": "donner_splash.svg"}}
{"name": "drag_selector", "arguments": {"selector": "#gear", "delta_x": 40, "delta_y": 0,
  "frames": 6, "include_display_frame": true}}
```

Unfamiliar document? Call `get_svg_source` first (or check `session_state`) to find a stable
id/class to target before `select_by_selector`/`drag_selector`/`transform_selector`.

Per-frame drag/transform results include three views — compare them to localize a stale
handoff:

- `composited_preview` — worker-side split tile list from `AsyncRenderer`.
- `display_preview` — what the editor-side tile cache would blit after the render lands.
- `display_before_render` — presentation state right after the input event, before the next
  async result. **This is the frame that catches stale cached-tile handoff bugs.**
  Transform frames add `translation_doc`, `document_from_cached_document`, and each tile's
  `effective_document_from_cached_document` for validating affine handoffs.

For real-GL pixels through MCP: `replay_rnr` with `"gl_readback": true`, `"gl_capture_frame": N`
(or `gl_capture_left_mousedown`), `"gl_crop": "document-canvas"`. With `"include_display_diff": true` (`replay_rnr` only — other tools silently ignore it), `differing_pixels` is the pixelmatch
count and `diff_*` PNGs are emitted. MCP-recorded `.rnr` files carry document-space pointer
coordinates (format v2+; `kReproFileVersion` in `donner/editor/repro/ReproFile.h` is the current
writer version) — pass `"gl_drive_document_input": true` when GL-replaying them, or the pointer
positions land in the wrong space. Drop to the CLI above when you need several exact frames or a
before/after PNG comparison.

Source editing is revision-guarded: call `get_svg_source`, pass its revision as
`expected_source_revision` to `edit_svg_source` so a changed draft rejects the edit instead of
applying offsets to the wrong bytes. Invalid XML is kept as the editable draft while the preview
stays on the last parsed revision (`preview_stale: true` in responses).

## Layer escalation

Classify the failure first from the capture:

- **Wrong geometry** — the right content, wrong place/scale (offset, jump, drift): transform or
  viewport math.
- **Stale payload** — right place, old pixels (pre-edit content, element pops back): cache
  invalidation or a late result accepted.
- **Missing payload** — checkerboard/blank where content should be: result rejected/never
  uploaded.
- **Backend binding** — impossible pixels (garbage, entirely wrong texture) while diagnostics
  look correct: texture/bind-group lifetime.

Then find the freshest correct layer and the first stale handoff (async result accepted, texture
uploaded, snapshot retired, bind group cached) — the regression test belongs at that handoff.
Full layer table: `docs/editor_visual_debugging.md`.

Vocabulary (grep target in parentheses):

- **Split tiles** — the compositor splits the document into layers around the promoted
  (selected/dragged) element and caches the underlay/overlay rasters; per-frame split tile lists
  come from `AsyncRenderer` (`CompositorController` in donner/svg/compositor/).
- **Metadata-only tile / miss** — a tile update that reuses an already-uploaded texture and only
  refreshes offset/generation metadata; a miss means the referenced payload was absent
  (`metadataOnlyMissCount`, `GlTextureCache`).
- **Retired snapshot** — a WGPU texture kept alive until the GPU frame sampling it completes;
  early release shows garbage or wrong-scale content (`RetiredSnapshot` in
  donner/editor/GlTextureCache.h, design doc 0037).
- **Canvas epoch** — generation of the viewport's desired canvas size; late worker results from
  an old epoch must be rejected, not blitted (gate in `RenderCoordinator`).

Unit-test a boundary before reaching for a full replay test (all in `donner/editor/tests/`):

- Viewport math: `RenderPaneViewport_tests.cc`
- Pan/zoom gestures: `RenderPaneGesture_tests.cc`
- Click/hit-test: `RenderPaneClick_tests.cc`
- Async result acceptance: `AsyncRenderer_tests.cc`
- Texture identity/lifetime: `GlTextureCache_tests.cc`
- Presenter geometry: `PresentedFrameComposer_tests.cc`
- Full visual replay: `GlRnrReplay_tests.cc` (expensive, timing-sensitive — integration
  coverage, not the primary regression)

Pixel assertions use `donner/editor/tests:bitmap_golden_compare` + pixelmatch identity params —
never percentage thresholds and never a private comparator (see donner-pixel-diff). Crop tightly
around the suspect element.

## Failure signature → likely cause

| Symptom                                                  | Likely cause / first check                                                                                                                                                               |
| -------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Overlay jumps away from dragged content for one frame    | Active vs displayed drag preview mismatch (`activeDragPreview` vs `displayedDragPreview` in the readback); current overlay published over stale split tiles from an old canvas epoch     |
| Checkerboard flash behind layers                         | Stale split tiles vs new desired canvas size; late worker result rejected by UI but worker metadata advanced                                                                             |
| Texture at drastically wrong scale                       | NOT a coordinate bug — duplicate live texture handles, metadata-only tile aliasing, ImGui WGPU bind group cached by raw `ImTextureID`, retired snapshot released early (design doc 0037) |
| Element pops back to old position after dragging another | Compositor source sync; filtered-layer compose offsets; cached raster of pre-drag DOM composed with wrong offset                                                                         |

`EditorShell::layerInspectorStatusForReadback()` exposes the diagnostics
(`viewportDesiredCanvas` / `documentCanvas` / `compositorCanvas`, `duplicateLiveTextureCount`,
`metadataOnlyMissCount`, `activeDragPreview` / `displayedDragPreview`, per-tile
generation/offset/handle — see the struct in donner/editor/EditorShell.h). If diagnostics look
correct but pixels are impossible, move down to texture/backend lifetime.

## Deterministic replay for committed tests

API: `RunGlRnrReplay(options, &result, &error)` in `donner/editor/repro/GlRnrReplay.h`. For
byte-identical captures set `workerScheduling = GlRnrReplayWorkerScheduling::DrainEachFrame` and
`contentOnlyCapture = true`; `HoldFramesBehind` + `holdFramesBehind = n` models a worker held n
frames behind; `AsyncRenderer::setReplayRenderDelayForTesting(delay)` proves a test would catch
a timing regression. Full semantics: `docs/deterministic_replay_testing.md`.

Traps:

- **Never drain on `AsyncRenderer::isBusy()`** — a staged `DoneState` result is "busy" but not
  in flight, so that wait hangs forever. Wait on `hasRenderInFlightForTesting()` and bound every
  wait with a deadline.
- Content-only capture is a readback-only presenter mode — it must not clear overlay textures or
  skip overlay rasterization, or the next non-capture frame is perturbed.
- `GlRnrReplay_tests.cc` is timing-sensitive; under `--config=geode` on a
  remote-exec-by-default setup run it with `--strategy=TestRunner=local` (see the comment in
  `donner/editor/tests/BUILD.bazel`).
- Mirror the exact request-posting sequence the real editor fires — do not fabricate a prewarm
  phase that production never runs, or the test verifies a fictional pipeline.

## Related

- Sibling skills: donner-bugfix-discipline (red-to-green rules), donner-pixel-diff (comparison
  helpers), donner-geode-backend (WebGPU/Geode internals), donner-rendering-pipeline,
  donner-build-test.
- Docs: `docs/editor_visual_debugging.md`, `docs/deterministic_replay_testing.md`,
  `docs/design_docs/0036-composited_presentation_retrospective.md`,
  `docs/design_docs/0037-geode_presentation_glitch_investigation.md`,
  `tools/mcp-servers/editor-control/README.md`.
