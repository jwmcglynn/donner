# Donner Editor

The Donner editor is a browser-based and desktop SVG editor built on top of the
Donner SVG engine. It uses a sandbox architecture to isolate the parser child
from the host, communicating via a wire format that the `ReplayingRenderer`
decodes onto whatever `RendererInterface` backend was selected at build time.

## Building

### Desktop (default — tiny-skia backend)

```sh
bazel build //donner/editor/gui:donner_editor_gui
bazel run //donner/editor/gui:donner_editor_gui
```

### Desktop — Geode (WebGPU) backend

```sh
bazel build --config=geode //donner/editor/gui:donner_editor_gui
bazel run --config=geode //donner/editor/gui:donner_editor_gui
```

### WASM (default — tiny-skia backend)

```sh
bazel build --config=editor-wasm //donner/editor/wasm:wasm
bazel run --config=editor-wasm //donner/editor/wasm:serve_http
```

### WASM — Geode (WebGPU) backend

```sh
bazel build --config=editor-wasm-geode //donner/editor/wasm:wasm
bazel run --config=editor-wasm-geode //donner/editor/wasm:serve_http
```

## Renderer backend selection

The editor uses the backend-agnostic `Renderer` wrapper
(`donner/svg/renderer/Renderer.h`). The active backend is selected entirely at
build time via the `--//donner/svg/renderer:renderer_backend` flag. No runtime
flags or C++ code branches are needed:

| Config | Backend |
|--------|---------|
| *(default)* | TinySkia — lightweight software rasterizer |
| `--config=geode` | Geode — experimental WebGPU + Slug |

## Running tests

```sh
bazel test //donner/editor/...
```
