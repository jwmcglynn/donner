# Design: WASM iOS Runtime Compatibility

**Status:** In Progress
**Author:** GPT-5
**Created:** 2026-07-10

## Summary

The web editor ships one static package that selects the best available renderer at runtime. Safari
26 and newer use the Geode WebGPU backend when WebGPU exposes an adapter. Safari 15.4 and newer
without WebGPU fall back to the TinySkia WebGL2 backend. Both paths retain the existing pthread
build and therefore require HTTPS plus cross-origin isolation. Header-capable hosts provide COOP and
COEP directly; secure static hosts may establish the same policy with the packaged service worker.

This design covers runtime startup and renderer selection on iPhone-class browsers. Responsive
layout, touch-first controls, mobile toolbars, and other user-interface changes are separate work.

## Goals

- Start the editor on iOS Safari when the browser exposes `SharedArrayBuffer` and WebGL2.
- Prefer Geode when a WebGPU adapter is available and use TinySkia otherwise.
- Keep one immutable deployment candidate containing both reviewed renderer binaries.
- Start on secure static hosts that cannot configure response headers.
- Preserve the desktop editor layout and interaction model unchanged.
- Detect capabilities instead of parsing the browser user agent.
- Fail clearly before loading Wasm when required browser capabilities are absent.

## Non-Goals

- Responsive or touch-first editor UI.
- Supporting Safari older than 15.4. Safari 15.2 adds the cross-origin isolation required by this
  pthread build; Safari 15.4 adds the narrow `wasm-unsafe-eval` CSP source used by the host.
- Replacing pthreads with a single-threaded build.
- Making Geode run where Safari does not expose WebGPU.
- Supporting insecure non-localhost origins.
- Loading cross-origin subresources that do not opt into the embedder policy.

## Next Steps

- Complete the release gates and real-device startup check.

## Implementation Plan

- [x] Milestone 1: Define and implement runtime backend selection.
  - [x] Add red unit tests for WebGPU preference, fallback, overrides, and unsupported browsers.
  - [x] Add a capability selector that returns only `geode/` or `tiny_skia/` package roots.
  - [x] Move bootstrap code and CSS out of inline HTML so a strict CSP needs no inline exceptions.
- [x] Milestone 2: Build one compatible static package.
  - [x] Add a deterministic staging tool that keeps each backend in its own directory.
  - [x] Add a same-origin service-worker fallback for secure hosts without configurable headers.
  - [x] Include both backend binaries and their hashes in deployment provenance.
- [x] Milestone 3: Add iOS-shaped browser verification.
  - [x] Run the combined package under Chromium with automatic Geode selection.
  - [x] Run automatic TinySkia fallback under Playwright WebKit with an iPhone device profile.
  - [x] Prove WebKit TinySkia document pixels at the desktop calibration without changing mobile
        layout.
  - [x] Assert secure context, cross-origin isolation, shared memory, WebGL2, backend selection, and
        successful runtime initialization.
- [ ] Milestone 4: Complete release gates.
  - [ ] Run both existing backend browser suites, the full Bazel gate, CMake validation, and format
        checks.
  - [ ] Run fuzzing and sanitizer workflows for the exact source revision.
  - [ ] Perform an independent review before publishing a pull request or deployment candidate.

## Proposed Architecture

The two existing Bazel web packages remain independently testable. A deterministic staging tool
combines their byte-identical shared files with two immutable backend directories:

```text
index.html
editor.css
editor-bootstrap.js
enable-threads.js
backend-selector.js
donner_icon.svg
geode/editor.js
geode/editor.wasm
tiny_skia/editor.js
tiny_skia/editor.wasm
```

`backend-selector.js` first honors the bounded `renderer=geode|tiny_skia` diagnostic override. In
automatic mode it requests a WebGPU adapter and selects Geode only when the request succeeds. It
otherwise verifies WebGL2 and selects TinySkia. The selected glue file resolves its Wasm file
relative to its own backend directory.

The preferred host configuration sends `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. When those headers are absent on a secure origin,
`enable-threads.js` registers as a same-origin service worker, adds the same headers to successful
responses, and reloads once it becomes active. It is a no-op when the document is already isolated.
The bootstrap refuses to load either backend when `SharedArrayBuffer` remains unavailable. Static
HTML contains no inline script, inline event handler, or inline style, allowing a default-deny CSP
with self-hosted script and style exceptions only.

## Security / Privacy

SVG, XML, CSS, and font inputs remain untrusted. This work does not change parser surfaces or grant
new network access. The service worker is packaged on and restricted to the editor origin. It
forwards requests without caching or rewriting bodies and does not modify opaque responses. The
static host permits only GET and HEAD, serves the candidate from one origin, and applies a
default-deny CSP. `script-src` admits same-origin scripts and `wasm-unsafe-eval`; `worker-src` admits
same-origin and blob workers required by Emscripten pthreads. `connect-src`, `img-src`, and
`style-src` stay same-origin or narrower.

The selector accepts only two literal backend names. Unit tests reject other values. The combined
candidate is produced once in CI, records each file hash, and is promoted without rebuilding.

## Testing and Validation

- `node --test donner/editor/wasm/tests/backend-selector.spec.mjs` enforces deterministic selection
  and unsupported-capability errors.
- `node --test donner/editor/wasm/tests/isolation-fallback.spec.mjs` verifies registration, direct
  header-host no-op behavior, and the service worker's COOP/COEP response policy.
- The Editor WASM workflow serves the compatible package without host-provided isolation headers
  and verifies the service-worker reload and runtime startup in Chromium, Firefox, and WebKit.
- Existing Chromium suites continue to verify both Geode presentation and TinySkia pixels.
- A Playwright WebKit project uses a real iPhone viewport and device pixel ratio to verify automatic
  TinySkia fallback and stable runtime initialization. A second DPR-1 presentation case verifies
  document pixels at the desktop calibration without claiming responsive UI support.
- The Editor WASM workflow stages the combined package only after all three browser lanes pass.
- `bazel test //...`, CMake generation validation, fuzzing, ASan, and UBSan remain release gates.

## Rollout Plan

The combined package replaces the Geode-only deployment candidate. Hosts either provide the required
COOP/COEP headers or allow the packaged same-origin service worker to control the editor scope.
Rollback reactivates the prior reviewed candidate. Deployment proceeds only after an actual iPhone
opens the candidate and confirms runtime startup; touch and layout findings are recorded for the
later UI pass.
