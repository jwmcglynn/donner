# Donner Editor

The Donner editor is a browser-based and desktop SVG editor built on top of the
Donner SVG engine. The interactive editor parses and renders in-process through
the selected `Renderer` backend.

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
