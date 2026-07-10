# Donner Editor

The Donner editor is a browser-based and desktop SVG editor built on top of the
Donner SVG engine. The interactive editor parses and renders in-process through
the selected `Renderer` backend.

## Visual design language

The editor uses the shared Graphite design language documented in
[Editor Design Language](../../docs/editor_design_language.md). `EditorTheme`
owns surfaces, text, Signal Teal interaction state, semantic colors, spacing,
radii, and fixed control dimensions. Custom canvas chrome reads the active
theme instead of introducing widget-local colors.

When extending the UI:

- reuse `EditorTheme` tokens instead of raw product-chrome color literals
- keep spacing and fixed control dimensions on the 4 px grid
- use Donner-rendered SVG assets for tool icons
- keep debug telemetry in explicit diagnostics surfaces
- update the focused theme or visual replay coverage for changed contracts

The source pane is collapsed on startup behind the left reveal rail. Source-navigation commands
open it automatically. Transform controls use responsive Position, Size, and Rotation rows; the
advanced matrix remains available through its disclosure. Numeric fields drag to adjust and enter
text mode on a simple click-release. Tool and cursor SVGs use black cores with white halos so they
remain legible over light and dark content. The toolbar exposes only ready tools; unfinished path
editing remains hidden until its interaction is complete.

New text inherits the current fill. Selecting or editing one text element opens a compact floating
font toolbar below the canvas tools. Point-text frames use stable font em-box height, reveal on
pointer movement, and fade while typing. Frame resize keeps its preview on the UI thread and performs
DOM rewrap and source writeback once on release; text and select handles share the same dimensions.

## Building

### Desktop (default - Geode/WebGPU backend)

```sh
bazel build //donner/editor
bazel run //donner/editor -- donner_splash.svg
```

The editor target applies a Geode transition internally, so no `--config=geode`
flag is needed for the desktop editor.

### Desktop - TinySkia backend

```sh
bazel build //donner/editor:editor_impl
```

`editor_impl` is the untransitioned implementation target and inherits the
command-line renderer backend. It is primarily useful for backend debugging.

### WASM (default - Geode/WebGPU backend)

```sh
bazel build --config=editor-wasm //donner/editor/wasm:wasm
bazel run --config=editor-wasm //donner/editor/wasm:serve_http
```

## Renderer backend selection

The editor uses the backend-agnostic `Renderer` wrapper
(`donner/svg/renderer/Renderer.h`). The public desktop editor target selects
Geode at build time with a Bazel transition; the editor WASM config also selects
Geode by default. No runtime flags or C++ code branches are needed:

| Target / config               | Backend                             |
| ----------------------------- | ----------------------------------- |
| `//donner/editor`             | Geode - WebGPU + Slug               |
| `--config=editor-wasm`        | Geode - browser WebGPU              |
| `//donner/editor:editor_impl` | Inherits command-line renderer flag |

Desktop presentation is also selected at build time:

- Geode uses a GLFW `GLFW_NO_API` WebGPU host. `RendererGeode` exports
  `RendererGeodeTextureSnapshot` payloads, and the editor presents their
  `WGPUTextureView`s directly through ImGui WGPU. Normal Geode editor
  presentation intentionally does not fall back to `takeSnapshot()` or GL
  texture upload.
- TinySkia debugging builds use the existing GLFW OpenGL host. Renderer outputs
  are CPU `RendererBitmap` payloads that `GlTextureCache` uploads to GL textures.

## Running tests

```sh
bazel test //donner/editor/...
```

Run the Inspector UI fuzzer under AddressSanitizer with:

```sh
bazel run --config=asan-fuzzer //donner/editor/tests:inspector_ui_fuzzer -- \
  -max_total_time=30 -max_len=4096 -timeout=5
```
