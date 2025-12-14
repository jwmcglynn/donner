# Native Geode Canvas Shim (Dawn)

This directory contains the starter C++ shim for the Canvas-like API used by the geode renderer when
running natively on Linux or macOS. It mirrors the JavaScript `GeodeCanvas` surface from the web
prototype but leaves GPU plumbing to Dawn and the upcoming BCR `babylon` WebGPU/WASM bridge.

## Status
- Defines the C++ API surface for a Dawn-backed geode canvas, including path commands, basic state,
  and PNG readback for offscreen renders.
- Records path commands and draw intents (fill/stroke) and encodes them to geode segments with
  per-draw bounds so GPU buffers can be populated when the Dawn device wiring lands.
- Packs encoded draws into GPU-ready buffers (geode storage + per-draw uniforms) so the remaining
  Dawn wiring can map buffers to swapchain or offscreen textures.
- Builds a Dawn render plan with uniform-buffer dynamic offsets aligned to 256 bytes per draw,
  ready to bind into a render pass once device/swapchain plumbing is connected.
- Exposes Dawn submission metadata (buffer usages, sizes, and render target selection) so device
  code can create buffers and bind the right swapchain or offscreen texture view without inspecting
  encoder internals.
- Provides helpers that translate submission metadata into Dawn buffers and render targets, creating
  offscreen RGBA textures when no swapchain view is supplied.
- Supplies WGSL-backed pipeline/bind-group creation and render-pass encoding so encoded draws can be
  submitted to Dawn devices once upload data is bound.
- Uploads packed geode segments/uniforms and submits encoded draws to a Dawn queue for execution.
- Adds offscreen readback and PNG encoding helpers so headless runners can emit golden captures
  without browser dependencies.
- Implementation is otherwise stubbed; upcoming steps will wire the shim to Dawn devices, load the
  shared WGSL modules, and hook up swapchain/offscreen presentation.

## GPU Upload Layout
- `GeodeCanvas::prepareGpuUpload` flattens all recorded draws into a contiguous geode buffer.
  The layout matches the web prototype (32-byte segments: three points plus a kind tag).
- Per-draw uniforms carry bounds, viewport dimensions, stroke width, a fill/stroke flag, and
  segment offsets/counts so Dawn can bind a single storage buffer and iterate draws.

## Next Steps
- Connect `CreateDawnGeodeCanvas` to a real Dawn device and queue, sharing WGSL modules with the web
  build.
- Wire the readback helpers into swapchain/offscreen runners to emit PNGs for regression capture.
- Integrate with the BCR `babylon` bindings to avoid custom JS glue when targeting WASM.
