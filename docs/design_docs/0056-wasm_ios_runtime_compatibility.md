# Design: Geode-only Web Editor Runtime

**Status:** In Progress
**Author:** GPT-5
**Created:** 2026-07-10
**Revised:** 2026-07-28

## Summary

The web editor ships one Geode WebGPU binary. The earlier dual-renderer package and runtime
software fallback are retired. An editor Wasm build with any renderer other than Geode fails during
Bazel analysis, before compilation or packaging. This keeps renderer behavior, texture ownership,
thumbnails, browser verification, and release provenance on one implementation.

The pthread build still requires HTTPS plus cross-origin isolation. Header-capable hosts provide
COOP and COEP directly; secure static hosts may establish the same policy with the packaged service
worker.

## Goals

- Ship one immutable editor package containing only Geode.
- Render the canvas, overlays, carousel images, and Layers panel thumbnails through Geode.
- Fail at Bazel analysis if an enabled editor Wasm target selects another renderer.
- Preserve editor text and embedded UI fonts.
- Start only when WebGPU, shared memory, and cross-origin isolation are available.
- Keep browser pixel regressions tied to the renderer users actually run.

## Non-Goals

- Removing the software renderer from non-editor libraries or desktop-only test configurations.
- Providing a software fallback for browsers without WebGPU.
- Replacing pthreads with a single-threaded build.
- Responsive or touch-first editor UI.

## Architecture

The package is flat and has one runtime:

```text
index.html
editor.css
editor-bootstrap.js
enable-threads.js
worker-surface-selector.js
donner_icon.svg
editor.js
editor.wasm
```

The packaged selector publishes `geode` with the package root as its asset base. There is no runtime
renderer query, alternate binary directory, or backend fallback. The editor binary always links the
browser WebGPU bridge and Geode runtime options. `editor_wasm_geode_only_guard` reads the editor-Wasm
enable flag and renderer build setting; it fails analysis unless the renderer is `geode`.

Layers panel thumbnails use Geode texture snapshots. They do not read back a CPU bitmap and upload
it again. A thumbnail pass may use the current canvas presentation version as proof that the same
document version is safe to render even while the document retains render invalidation. Older
presentation versions remain blocked.

## Build and Packaging Contract

- `bazel run //donner/editor/wasm:serve_http` applies the editor transition automatically.
- `--config=editor-wasm` selects Geode, text support, pthreads, and size optimization.
- The package size test rejects software-renderer names in shipped JavaScript.
- The editor workflow builds, tests, stages, and records provenance for one package and one Bazel
  config.
- Non-editor renderer targets retain their existing backend choices.

## Testing and Validation

- `//donner/editor/wasm:editor_wasm_transition_tests` proves the default run transition selects the
  complete Geode configuration and runtime options.
- A negative build with editor Wasm enabled and another renderer selected must fail with the
  Geode-only analysis error.
- `//donner/editor/wasm:wasm_geode_package_size_tests` proves the package size and forbidden-token
  contract.
- Node contract tests prove the flat package selects Geode and loads root `editor.js` and
  `editor.wasm`.
- Firefox pixel tests prove every visible Donner Splash layer row receives a Geode texture
  thumbnail, including named nested rows.
- Chromium, Firefox, WebKit, and real Safari regressions exercise the Geode package.

## Security and Rollout

SVG, XML, CSS, and font inputs remain untrusted. This change removes one executable renderer and its
runtime selector from the editor package, reducing the shipped surface. The service worker remains
restricted to the editor origin and only establishes the isolation headers required by pthreads.

CI builds one immutable Geode candidate and records its source revision, package files, hashes,
dependency lock hash, Bazel config, and target. Promotion uses that candidate without rebuilding.
Browsers without required WebGPU support receive a clear capability failure. Rollback reactivates a
previously reviewed candidate.
