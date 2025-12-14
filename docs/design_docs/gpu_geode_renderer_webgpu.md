# Design: GPU-Accelerated Geode Renderer for WebGPU

## Summary
- Introduce a GPU-accelerated renderer that reproduces the distance-boundary glyph representation
  from the Geode technique. It targets WebGPU for native builds via Dawn first, with web support
  following once the desktop path is validated. The API mirrors HTML Canvas for standalone use on
  web and native (Linux/macOS) without requiring a browser runtime.
- Enables high-quality vector text and path rendering with resolution independence, low memory
  footprint, and portable backends; integration with Donner occurs later via the
  `AbstractRenderer` layer.

## Goals
- Deliver a WebGPU-based Geode renderer that matches the published quality goals: analytically
  defined boundaries with stable antialiasing.
- Provide a Canvas-like immediate mode API consumable by desktop clients first (Linux/macOS Dawn),
  with web parity following.
- Ship a translation layer that maps the Canvas API to Dawn on desktop (Linux/macOS) and leave the
  Vulkan backend scoped out for later once the Dawn path is stable.
- Supply a testable pipeline that can be validated with golden images and shader unit tests.

## Non-Goals
- Integrating the renderer into Donner or replacing existing backends (handled later via
  `AbstractRenderer`).
- Implementing full HTML Canvas feature parity (focus on 2D path/text primitives needed for Geode
  rendering).
- Reproducing browser event/input handling or DOM integration.

## Next Steps
- Stand up native (Linux/macOS) runners that invoke the renderer outside a browser, outputting
  windowed or offscreen surfaces for demos and tests.
- Wire the new PNG readback helpers into headless runners so swapchain and offscreen surfaces can be
  validated end-to-end.
- Capture golden outputs from the prototype to seed automated visual regression tests on native
  (first) and web (second).
- Harden AA and derivative estimation in WGSL as more glyph complexity is added.
- Expand the Canvas-like surface with gradients/text state to prep Dawn desktop parity.

## Implementation Plan
- [x] Milestone 1: Prototype WebGPU Geode pipeline
  - [x] Draft WGSL modules for Geode evaluation (field evaluation, edge blending, MSAA resolve).
  - [x] Build a minimal path-to-Geode encoder in JS/TS to feed GPU buffers.
  - [x] Render a single glyph in browser and capture golden output.
- [x] Milestone 2: Canvas-like API surface
  - [x] Specify TypeScript-friendly Canvas-like context (paths, text-ready hooks, state stack) that
        can be shared across web and desktop builds.
  - [x] Implement context methods that emit Geode buffers and draw calls on WebGPU.
  - [x] Require WebGPU/Dawn availability and drop CPU fallback paths; execution is GPU-only.
- [ ] Milestone 3: Dawn desktop translation (Linux/macOS)
  - [x] Define C++ shim that forwards Canvas API calls to Dawn equivalents and shares WGSL modules.
  - [x] Record path commands and draw state in the Dawn shim so they can be converted to Geode
        buffers.
  - [x] Encode recorded commands into Geode segments with per-draw bounds to prep GPU buffer upload.
  - [x] Pack encoded draws into GPU upload payloads (Geode storage + per-draw uniforms) for Dawn
        pipelines.
  - [x] Precompute Dawn render plans with 256-byte-aligned uniform offsets for dynamic binding.
  - [x] Surface Dawn submission metadata (buffer usages, sizes, and render targets) so device code
        can allocate and present without inspecting encoder internals.
  - [x] Implement buffer and texture interop, including swapchain presentation on desktop.
  - [x] Create Dawn pipelines and bind groups for geode submissions using WGSL sources shared with
        the web prototype, and encode render passes.
  - [x] Upload packed buffers and submit render passes to Dawn queues for swapchain/offscreen paths.
  - [x] Provide offscreen readback helpers that emit PNG-encoded buffers for headless captures on
        Linux/macOS.
  - [ ] Establish build configuration for desktop demos/tests on Linux and macOS, including
        headless/offscreen paths that emit PNGs for golden captures.
  - [ ] Replace ad-hoc JavaScript thunking with a BCR-published WebGPU/WASM bridge if one becomes
        available; otherwise, use Dawn's upstream WebGPU C/C++ headers with its WebAssembly build
        target to keep the desktop-first bridge consistent across native and wasm.
- [ ] Milestone 4: Web parity on browsers
  - [ ] Reuse the Dawn/WebAssembly path (or a BCR bridge if it lands) to run the Canvas API in
        browsers without custom JavaScript glue.
  - [ ] Align feature coverage and pixel baselines with the native pipeline.
- [ ] Milestone 5: Testing and tooling
  - [ ] Create golden image tests for representative glyphs and complex paths.
  - [ ] Add shader unit tests and property-based tests for coverage evaluation.
  - [ ] Automate CI steps for web (Playwright) and desktop (headless swapchain) pipelines.

## User Stories
- As a web developer, I can draw vector text and paths via a Canvas-like API and get
  crisp results on high-DPI displays.
- As a desktop app developer, I can reuse the same drawing API and shaders via Dawn without platform
  rewrite.
- As a performance engineer, I can validate perf and quality on native first, then on web once
  parity lands.

## Background
- The Geode technique encodes curve segments into compact GPU-friendly data that allows analytic
  evaluation of signed-distance-like coverage in the fragment shader, yielding
  resolution-independent edges and stable antialiasing. WebGPU support and Dawn parity provide
  portability across browsers and native.
- This design is a clean-room implementation built from publicly available descriptions of the Geode
  algorithm. The renderer targets the documented behavior without reusing any proprietary source
  code.

## Requirements and Constraints
- Rendering quality must match the analytic coverage approach described for Geode: sharp edges,
  smooth AA, and correct joins/caps.
- Support text (glyph outlines) and generic paths (quadratic/cubic Béziers, arcs). Filled shapes and
  stroke rendering with joins and caps are required; dash support can be deferred.
- Memory footprint should stay linear with path complexity; avoid per-pixel CPU work.
- Target platforms: desktop via Dawn (Linux/macOS) with no browser runtime requirement first;
  modern browsers with WebGPU once desktop stabilizes; Vulkan is scoped out until the Dawn path is
  complete and abstractions prove stable.
- Keep lines under 100 characters and avoid new dependencies unless critical.

## Proposed Architecture
- **Front-end (Canvas-like context):** TypeScript API resembling `CanvasRenderingContext2D` (path
  construction, state stack, transforms, fill/stroke, text). Operations accumulate a display list of
  path segments and style state.
- **Geode encoder:** Converts path segments and glyph outlines into packed Geode buffers (control
  points, edge flags, per-segment coverage params). Encoders shared between web and desktop via
  platform-neutral data schemas.
- **GPU pipeline:**
  - WGSL shader modules implementing the Geode evaluation algorithm: vertex stage expands instances;
    fragment stage evaluates coverage using analytic edge distance and blends coverage per pixel;
    optional MSAA resolve for tricky joins.
  - GPU resources: static uniform buffers for global params, storage buffers for Geode data, and
    small lookup textures for curves if needed.
  - Render pass targets swapchain texture on web and Dawn; backend abstraction separates command
    encoding from platform specifics.
- **Backend abstraction:** Thin interfaces for device, pipeline, buffer, texture, and swapchain;
  implemented initially for Dawn and WebGPU, with Vulkan deferred until desktop/web parity is
  proven.
- **Desktop translation (Dawn):** C++ shim implementing the Canvas-like API, forwarding to shared
  abstractions, loading WGSL modules, handling swapchain/presentation, and providing both windowed
  and offscreen entry points for Linux/macOS builds (including WebAssembly builds that share the
  Dawn codepath when a BCR WebGPU/WASM bridge is available).
- **Deferred Vulkan backend:** Add once the Dawn path is stable and abstractions are validated;
  reuse Geode encoders and pipeline setup.

## Clean-Room Drawing Contract
- **Resources and extraction:** Fonts/icons are converted offline into Geode resources with two GPU
  textures mirroring the publicly described model: a curve texture (float16/float32 control points)
  and a band texture (uint16 pairs of band indices). Resources are stored in an aligned blob;
  extraction is caller-provided-memory based to keep runtime allocation free and compatible with
  Dawn/WebAssembly builds.
  - **Vertex layout:** The renderer consumes five interleaved attributes that mirror the published
    vertex contract from the Geode materials: position (object position + normal for dilation),
    texcoord (em-space + packed data location), jacobian (2x2 inverse derivative mapping), banding
    (band transforms), and color. Offsets and stride follow the documented layout: position @ 0,
    texcoord @ 16, jacobian @ 32, banding @ 48, color @ 64 with 68- or 80-byte stride depending on
    color format.
  - **Geometry:** Start with one quad per glyph expanded by a half pixel for AA support; later
    enable a bounded-vertex polygon variant (up to eight vertices, six triangles) to reduce
    fragment work at large sizes. The vertex shader performs dynamic dilation using per-vertex
    normals and the object→clip transform so AA padding stays minimal under perspective.
  - **Fragment evaluation:** The fragment stage evaluates winding number for quadratic Bézier
    contours using the published root-eligibility lookup based solely on the sign bits of each
    curve's y values (bit masks 0x2E and 0x74). Eligible roots contribute fractional coverage via
    saturate() over a half-pixel window scaled by pixels-per-em. Rays in ±x (and optionally ±y) are
    averaged for stable coverage; band textures provide per-pixel curve lists with early-out sorting
    by extremal x.
- **Performance tunables:** Band count is bounded (1–32) and can be split per band to choose ± ray
  directions. A linear-curve flag enables a simplified shader for icon-only paths. Adaptive
  supersampling for extreme minification is optional future work.

## API / Interfaces
- `GeodeCanvasContext` (TS): `beginPath`, `moveTo/lineTo/quadTo/cubicTo/arc`, `fill`, `stroke`,
  `fillText/strokeText`, `setTransform`, `save/restore`, `createLinearGradient` (minimal subset).
- `GeodeRenderer` (TS): `draw(displayList, renderTarget, options)`; optional
  `enableBackend(name)` to select WebGPU/Dawn (Vulkan later, once reinstated).
- C++ shim: `GeodeCanvas` class mirroring TS API, bridging to Dawn devices and shared Geode
  encoders.
- Shader modules: WGSL sources shared via build tooling; SPIR-V variants generated only after Vulkan
  development resumes.

## Data and State
- Display list holds path commands and style attributes; flushed to Geode buffers per frame.
- GPU buffers: Geode segment buffer, per-draw uniform buffer, optional glyph atlas metadata.
- State stack tracks transforms, stroke width, join/cap settings, fill/stroke styles.
- Resource lifetime tied to context; swapchain textures managed per frame with double buffering.

## Error Handling
- API methods validate inputs (NaN checks, path degeneracy) and return explicit errors or throw in
  JS contexts; desktop shim returns status codes.
- Backend selection fails fast with clear errors when required WebGPU/Dawn features are missing.
- Logging hooks for shader compilation/link errors and device loss notifications.

## Performance
- Avoid CPU tessellation; rely on Geode evaluation per pixel.
- Batch drawing commands to minimize pipeline/state switches; reuse bind groups and pipelines.
- Use compact Geode encodings to improve cache locality and bandwidth; consider quantization where
  it does not harm quality.
- Measure with GPU timestamp queries and frame capture tools (WebGPU profiling, Dawn tracing).

## Security / Privacy
- Inputs are application-supplied paths and text; treat as untrusted for memory bounds and device
  stability.
- Defensive measures: bounds-check Geode buffer writes, validate indices, clamp resource sizes, and
  cap path complexity per frame to avoid GPU hangs.
- Handle device loss gracefully and avoid leaking sensitive data through uninitialized buffers by
  zeroing or clearing resources before reuse.
- For web builds, respect browser security constraints; do not expose shared memory or unsafe shader
  extensions.

## Testing and Validation
- Golden image tests comparing rendered output against reference images for glyphs, joins, caps,
  and complex paths; run in desktop (Dawn) harnesses on Linux/macOS first, then headless WebGPU
  (Playwright) once the browser path is re-enabled.
- Shader unit tests for coverage math using programmable tests in WGSL or host-side verification.
- Property-based tests for path-to-Geode encoder (monotonic coverage, bounds correctness).
- Performance benchmarks for typical scenes (text paragraphs, icon sets) with frame time budgets.
- Continuous validation across backends (WebGPU, Dawn; Vulkan when added) with backend selection
  smoke tests.

## Dependencies
- WebGPU standard APIs and WGSL; Dawn for desktop translation.
- WebAssembly/WebGPU bridge: BCR currently lacks a published WebGPU/WASM C++ bridge (e.g., `babylon`
  is not available). We will rely on Dawn's official WebGPU C/C++ headers and its WebAssembly build
  target. If a maintained BCR bridge appears, we can swap to it to reduce custom toolchain glue.
- Tooling: Playwright for web golden captures; Dawn validation layers for debugging; Vulkan SDK is
  deferred with the Vulkan backend.

## Rollout Plan
- Start with Dawn desktop demo behind a feature flag; re-enable the web path after desktop parity is
  validated; enable a Vulkan prototype only after Dawn/web abstractions stabilize.

## Alternatives Considered
- CPU-based tessellation to triangles: rejected due to higher memory use and resolution dependence.
- Signed distance field atlases: rejected for lower quality at small sizes and atlas management
  overhead.

## Open Questions
- How to handle text shaping and font loading across web/desktop while keeping Geode encoding
    consistent? (likely harfbuzz + font loader on desktop; browser APIs on web)
- Do we need an intermediate IR for display lists to share between JS and C++ shims, or is a
    JSON/CBOR schema sufficient?
- Should we precompute stroke extrusion for complex dashes or defer dashing to future work?

# Future Work
- [ ] Add optional GPU-based dashing and path effects.
- [ ] Explore subpixel positioning and hinting strategies for text.
- [ ] Integrate with Donner `AbstractRenderer` once available.
