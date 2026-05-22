# Editor Visual Debugging {#EditorVisualDebugging}

\tableofcontents

This guide covers visual debugging for the Donner editor stack, especially the
Geode direct-texture path. Use it when the editor shows one-frame glitches,
overlay jumps, stale drag content, checkerboard gaps, or a texture rendered at
the wrong scale.

The rule of thumb is: get a live repro first, then narrow the failure to the
lowest layer that can still explain the pixels.

## Stack Layers

Visual editor bugs usually cross several layers. Treat each layer as a separate
test boundary.

| Layer              | What It Owns                                                      | Typical Bugs                                                                   | Useful Proof                                               |
| ------------------ | ----------------------------------------------------------------- | ------------------------------------------------------------------------------ | ---------------------------------------------------------- |
| Input and viewport | Mouse state, pan, zoom, DPR, pane geometry                        | Wrong document point, resize race, zoom focal drift                            | `.rnr` frame data and viewport diagnostics                 |
| DOM and selection  | Mutations, drag preview, selection bounds                         | Overlay lags live DOM, stale hit-test bounds                                   | active drag diagnostics, selection label, source writeback |
| Compositor         | Layer segmentation, cached raster payloads, compose offsets       | Old element pops back, filtered layer offset, stale canvas epoch               | compositor state, tile metadata, pixel crop comparisons    |
| Async renderer     | Worker scheduling, cancellation, result publication               | Late result wins, rejected result leaves side effects                          | `RenderCoordinator` tests and frame history                |
| Texture cache      | GL/WGPU texture ownership and tile reuse                          | metadata-only tile aliases wrong payload, retired WGPU snapshot dies too early | `GlTextureCache` unit tests and texture handle diagnostics |
| Presenter          | Tile rectangle mapping and overlay composition                    | right texture at wrong rect, overlay/current viewport mismatch                 | `PresentedFrameComposer` tests and captured frames         |
| ImGui/backend      | `ImTextureID`, bind groups, command buffers, framebuffer readback | raw handle reused, stale bind group, wrong texture through correct quad        | WGPU backend audit plus replay screenshots                 |

## Repro Workflow

Start by proving the actual pixels. Logs and inspector text are useful, but they
are not enough for one-frame visual bugs.

1. Record or find a `.rnr` repro that includes the bad interaction.
2. Replay it with captured frames around the visible failure.
3. Inspect the PNGs before adding diagnostics, because diagnostics can change
   timing.
4. Add the smallest diagnostic that distinguishes the candidate hypotheses.
5. Convert the repro into a unit or replay test at the lowest reliable layer.

Example replay:

```sh
bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- \
  --rnr /private/tmp/repro.rnr \
  --svg donner_splash.svg \
  --out-dir /private/tmp/donner-editor-repro \
  --capture-frame 78 \
  --capture-frame 79 \
  --capture-frame 80 \
  --max-frame 81 \
  --crop document-canvas
```

Use `--crop document-canvas` for most visual debugging. Use full-frame capture
when the failure may involve pane layout, dialogs, sidebars, or framebuffer
size.

Use `--print-diagnostics` only after the bad pixels are already captured. It
prints per-frame JSON from the replay harness, including canvas freshness,
overlay dimensions, paint-order tiles, raster canvas size, tile offsets,
drag translations, texture handles, metadata-only reuse, and drag-target flags.

```sh
bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- \
  --rnr /private/tmp/repro.rnr \
  --svg donner_splash.svg \
  --out-dir /private/tmp/donner-editor-diagnostics \
  --capture-frame 79 \
  --max-frame 81 \
  --crop document-canvas \
  --print-diagnostics
```

For timing-sensitive failures, run both paced and unpaced replays:

- `--pace` / paced replay is closer to manual interaction and worker timing.
- Unpaced replay is faster and more deterministic, but it can hide races that
  need realistic frame spacing.

## Tooling

### Replay Harness

The shared replay API lives in `donner/editor/repro/GlRnrReplay.{h,cc}`. The CLI
wrapper is `//donner/editor/tests:editor_rnr_gl_replay`.

Use it for:

- capturing exact frames to PNG,
- replaying with a known SVG override,
- cropping to the document canvas,
- enabling test-only WGPU framebuffer readback,
- exporting per-frame diagnostics for assertions or manual inspection.

The production Geode editor still presents direct WGPU textures. Framebuffer
readback is a replay/test tool, not a production fallback.

### Pixel Comparisons

Use `donner/editor/tests:bitmap_golden_compare` helpers for image assertions.
Prefer identity checks when a bug is a one-frame pop or stale texture:

```cpp
tests::CompareBitmapToBitmap(actual, expected, "case_name",
                             tests::PixelmatchIdentityParams());
```

Crop aggressively. A small crop around the suspected element usually gives a
more stable regression than comparing the whole editor canvas.

### Diagnostics Readback

`EditorShell::layerInspectorStatusForReadback()` exposes test-only state for
the replay harness. It is useful for proving presentation state:

- `viewportDesiredCanvas`
- `documentCanvas`
- `compositorCanvas`
- `metadataOnlyMissCount`
- `duplicateLiveTextureCount`
- `overlayDimsPx`
- `overlayTextureHandle`
- paint-order tile list with tile kind, generation, dimensions, offsets,
  drag translation, texture handle, metadata-only flag, and drag-target flag

If a visual frame is wrong but diagnostics look correct, suspect a lower backend
layer: stale texture handle, bind group cache, command buffer lifetime, or
framebuffer readback.

### Unit Test Boundaries

Use focused tests before reaching for full replay tests:

- Viewport math: `RenderPaneViewport_tests.cc`
- Gesture/pan/zoom input: `RenderPaneGesture_tests.cc`
- Click/hit-test behavior: `RenderPaneClick_tests.cc`
- Async rendering and result acceptance: `AsyncRenderer_tests.cc`
- Texture cache identity and lifetime: `GlTextureCache_tests.cc`
- Presenter geometry: `PresentedFrameComposer` tests
- Full visual replay: `GlRnrReplay_tests.cc`

Replay tests are valuable, but they are expensive and timing-sensitive. When the
root cause is a policy or lifetime rule, put the primary regression at that
layer and keep the replay as integration coverage.

## Failure Signatures

### Overlay Jumps

Symptom: the path outline or selection chrome appears at a different position
from the dragged content for one frame.

Check:

- active drag preview versus displayed drag preview,
- current viewport versus overlay raster canvas,
- whether the overlay was uploaded immediately over stale split tiles,
- whether the dragged tile uses a cached baseline plus the same drag delta as
  the overlay.

Useful proof:

- compare the active drag frame to the immediate mouse-up frame with a tight
  crop,
- assert `overlayDimsPx` is compatible with `viewportDesiredCanvas` before
  publishing a current overlay over split tiles.

### Checkerboard or Missing Background

Symptom: the document pane shows checkerboard behind some layers for one frame.

Check:

- whether stale split tiles remain visible while the viewport has moved to a new
  desired canvas size,
- whether a late worker result was rejected by the UI but still advanced
  worker-side publication metadata,
- whether `RenderCoordinator` keeps a usable full-canvas fallback while split
  tiles catch up.

Useful proof:

- frame diagnostics where `viewportDesiredCanvas` differs from tile
  `rasterCanvasSize`,
- `RenderCoordinator` tests that reject stale split previews but allow
  full-canvas stretch.

### Wrong Texture at Drastically Wrong Scale

Symptom: a tiny element or letter tile appears stretched across a large part of
the document or viewport for one frame.

This is usually not a pure coordinate bug. A coordinate bug moves or scales the
right payload. A wrong-scale texture splat often means the presenter supplied a
large quad while the backend sampled a stale small texture.

Check:

- duplicate live texture handles across tile IDs,
- metadata-only tile reuse against cached texture identity,
- WGPU snapshot lifetime after replacement,
- ImGui WGPU bind-group caching by raw `ImTextureID`,
- whether a released `WGPUTextureView` handle can be reused for a different
  snapshot.

Useful proof:

- a screenshot showing old small texture content through current large geometry,
- diagnostics where presenter geometry is plausible but texture content is
  impossible,
- a texture-cache lifetime test that holds retired snapshots across
  frames-in-flight and evicts backend bindings before releasing handles.

### Element Pops Back After Drag

Symptom: after dragging one element, clicking or dragging another makes the
first element appear at an old position for one frame.

Check:

- compositor source synchronization,
- filtered layer compose offsets,
- metadata-only tile geometry updates,
- whether the cached raster payload represents pre-drag DOM with a compose
  offset or post-drag DOM at identity.

Useful proof:

- crop the previously dragged element and compare first-click frame against a
  settled frame,
- compute simple feature metrics, such as a color centroid, when full identity
  is too strict for antialiasing.

## Root-Cause Pattern

Use this sequence for new bugs:

1. **Classify the visual failure.** Is it wrong geometry, stale payload, missing
   payload, or backend binding?
2. **Identify the freshest correct layer.** If diagnostics show correct tile
   geometry but pixels are impossible, move down to texture/backend lifetime.
3. **Find the first stale handoff.** Look for a result that changes ownership:
   async result accepted, texture uploaded, snapshot retired, bind group cached,
   draw command submitted.
4. **Add the regression at that handoff.** A replay screenshot may prove the bug,
   but a lower-level test usually prevents the exact class from returning.
5. **Rerun the live repro.** The screenshot that proved the bug should become
   clean before the issue is considered fixed.

## Known Root Causes

- **Stale split preview:** a late split-tile result from an old canvas epoch was
  accepted after zoom. Fix at `RenderCoordinator`: reject split previews whose
  tile raster canvas does not match the current desired canvas.
- **Overlay over stale content:** immediate overlay upload could publish
  current-canvas chrome over stale split tiles. Fix at `RenderCoordinator`: gate
  immediate overlay publication against the visible tile canvas epoch.
- **Metadata-only tile aliasing:** a tile update without payload could reuse a
  cached texture with insufficient identity. Fix at `GlTextureCache`: require
  kind, generation, texture dimensions, and raster canvas size to match.
- **WGPU texture-view reuse:** ImGui WGPU cached bind groups by raw
  `WGPUTextureView`/`ImTextureID`, while `GlTextureCache` released retired
  snapshots by upload churn. Fix at the WGPU presentation boundary: age retired
  snapshots by UI frame and evict the ImGui bind group before releasing a
  texture view.
- **Filtered layer compose offset:** filtered content could be recomposed with a
  stale source/offset after drag. Fix at the compositor/source-sync layer and
  protect with cropped replay comparisons.

## Related Docs

- [Editor responsiveness design](design_docs/0033-editor_design_tool_responsiveness.md)
- [Composited presentation retrospective](design_docs/0036-composited_presentation_retrospective.md)
- [Geode presentation glitch investigation](design_docs/0037-geode_presentation_glitch_investigation.md)
- [Geode renderer design](design_docs/0017-geode_renderer.md)
