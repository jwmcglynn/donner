# Design: Renderer Interface and Multi-Backend Support

**Status:** Shipped (Phases 1-2a, CMake backend selection; Phase 2b-4 planned as future work)
**Author:** Claude Opus 4.6
**Created:** 2026-03-05

## Summary

The renderer interface decouples SVG document traversal from backend-specific drawing, enabling
multiple rendering backends behind a consistent API. Phase 1 extracted a backend-agnostic
`RendererDriver` and `RendererInterface` from the monolithic full-Skia renderer, preserving Skia
behavior and resvg parity. Phase 2a shipped a lightweight `tiny-skia-cpp` backend, Bazel backend
selection, and a single-backend test architecture so normal test runs compile exactly one renderer
backend at a time.

The remaining work is focused on Phase 2b parity and developer ergonomics: completing TinySkia's
unsupported features, running the broader renderer suites under `--config=tiny-skia`, adding CMake
backend selection, and then implementing the recording and structural-test backends.

## Goals

- Decouple traversal logic from backend-specific drawing behind `RendererInterface`.
- Provide a Bazel-native and CMake-friendly mechanism for consumers to select a rendering backend
  at build time.
- Ship a lightweight `tiny-skia-cpp` backend as an alternative to the full Skia dependency.
- Ship a recording backend that serializes draw calls to a file for offline replay and debugging.
- Ship a GMock-integrated backend enabling `EXPECT_CALL`-style assertions on rendered output.
- Preserve existing `SVGRenderer` behavior, public API surface, and test coverage.

## Non-Goals

- Changing SVG parsing, layout, or styling stages.
- Altering public APIs for document setup or resource loading.
- GPU-accelerated backends (future work).
- Runtime backend switching (backend is selected at build time).

## Next Steps

- Run the broader renderer suites, especially `resvg_test_suite`, under `--config=tiny-skia` and
  document the remaining parity gaps.
- Decide whether TinySkia should grow text support, broader filter support, or remain an explicitly
  reduced-feature backend.
- Implement `RendererRecorder` and `MockRendererInterface` once backend selection and parity are
  stable.

## Implementation Plan

### Phase 1: Interface extraction and Skia backend (shipped)
- [x] Add `RendererInterface.h` with frame control, state stack, paint, primitive, compositing,
  mask, pattern, and snapshot APIs.
- [x] Extract traversal code into `RendererDriver` with deferred layer management.
- [x] Implement all driver features: masks, patterns (with nesting), markers, filters, deferred
  pops, `subtreeConsumedBySubRendering`.
- [x] Adapt the full-Skia renderer to implement `RendererInterface` with full feature parity.
- [x] Wire entry points (`SVGRenderer`, tooling, viewer, tests) through `RendererDriver`.
- [x] Add mock-based driver interaction tests (`renderer_driver_tests`).
- [x] Achieve full resvg test suite parity (660 tests, 0 regressions).

### Phase 2: tiny-skia-cpp backend
- [x] Land `third_party/tiny-skia-cpp` and add the Bazel `cc_library` target.
- [x] Create `RendererTinySkia` implementing `RendererInterface`.
  - [x] Implement frame control: allocate pixel buffer, clear background.
  - [x] Implement transform stack.
  - [x] Implement clip stack.
  - [x] Implement `drawPath`/`drawRect`/`drawEllipse` with fill and stroke.
  - [x] Implement gradient resolution.
  - [x] Implement `pushIsolatedLayer`/`popIsolatedLayer` for opacity compositing.
  - [x] Implement pattern tile recording and tiled replay.
  - [x] Implement mask compositing.
  - [x] Implement `drawImage`.
  - [x] Implement `takeSnapshot` returning `RendererBitmap` from the pixel buffer.
  - [ ] Implement `drawText`.
  - [ ] Implement full filter-effect parity. The current backend only supports a limited subset and
        emits verbose warnings for unsupported cases.
- [x] Add Bazel backend selection and make `//donner/svg/renderer:renderer` resolve to exactly one
  concrete backend per build (see [Backend Selection](#backend-selection)).
- [x] Refactor renderer tests to run one backend per build using shared goldens instead of direct
  backend-vs-backend comparison.
- [x] Split Skia-only ASCII snapshot tests into backend-incompatible Bazel targets so
  `bazel test --config=tiny-skia //...` does not build Skia just for those tests.
- [x] Add CMake backend selection.
- [ ] Run `resvg_test_suite` against `RendererTinySkia` and document parity gaps.

### Phase 3: Recording backend
- [ ] Implement `RendererRecorder` wrapping another `RendererInterface` (tee pattern).
  - [ ] Record each interface call to an in-memory list of `DrawCall` variant structs.
  - [ ] Forward all calls to the wrapped backend for live rendering.
- [ ] Implement text output of the recorded call list for observability/debugging.
  - [ ] Human-readable format: one line per call with indentation for push/pop nesting.
  - [ ] Include key parameters (transform values, clip rects, paint colors, path bounds).
- [ ] Implement replay: iterate recorded calls and invoke them on any `RendererInterface`.
- [ ] Add round-trip tests: render, record, replay, compare bitmaps.

### Phase 4: GMock-integrated backend
- [ ] Create `MockRendererInterface` using `MOCK_METHOD` for all `RendererInterface` methods.
- [ ] Create `RendererExpectations` helper with high-level matchers:
  - [ ] `ExpectPath(path_matcher, stroke_matcher)` for draw call assertions.
  - [ ] `ExpectTransform(transform_matcher)` for transform stack assertions.
  - [ ] `ExpectClip(clip_matcher)` for clip assertions.
  - [ ] `ExpectIsolatedLayer(opacity_matcher)` for compositing assertions.
  - [ ] `ExpectMask()`/`ExpectPattern()` for mask/pattern lifecycle assertions.
- [ ] Add `RenderAndExpect` test fixture that parses SVG, drives rendering, and verifies
  expectations.
- [ ] Port a representative subset of resvg tests to structural assertions.
- [ ] Document patterns for writing new structural rendering tests.

## Background

Donner's renderer was originally a monolithic full-Skia renderer implementation that interleaved
document traversal with Skia-specific drawing. This made it impossible to test traversal logic
independently, swap backends, or assert on rendering structure without pixel comparison.

The branch name `tiny-skia` reflects the original motivation: enabling a lightweight alternative
to the ~50MB Skia dependency. The `tiny-skia-cpp` library at
`github.com/jwmcglynn/tiny-skia-cpp` provides a C++ port of the Rust
[tiny-skia](https://github.com/nickel-org/tiny-skia) library with a minimal API surface suitable
for SVG rendering.

## Proposed Architecture

### Component Overview

```
SVGDocument
    |
    v
RendererDriver  ----> RendererInterface (abstract)
                            |
                +-----------+-----------+-----------+
                |           |           |           |
          FullSkiaRenderer  RendererTinySkia  Recorder  MockRenderer
```

### RendererInterface

Located at `donner/svg/renderer/RendererInterface.h`. The interface is Skia-free and uses only
Donner primitives:

```cpp
class RendererInterface {
 public:
  virtual ~RendererInterface() = default;

  // Document rendering and dimensions
  virtual void draw(SVGDocument& document) = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;

  // Frame lifecycle
  virtual void beginFrame(const RenderViewport& viewport) = 0;
  virtual void endFrame() = 0;

  // Transform stack: setTransform replaces the matrix (absolute entity transforms),
  // push/popTransform use save/concat/restore (local relative transforms).
  virtual void setTransform(const Transform2d& transform) = 0;
  virtual void pushTransform(const Transform2d& transform) = 0;
  virtual void popTransform() = 0;

  // Clip stack
  virtual void pushClip(const ResolvedClip& clip) = 0;
  virtual void popClip() = 0;

  // Compositing layers
  virtual void pushIsolatedLayer(double opacity) = 0;
  virtual void popIsolatedLayer() = 0;
  virtual void pushFilterLayer(std::span<const FilterEffect> effects) = 0;
  virtual void popFilterLayer() = 0;

  // Mask compositing (3-layer technique: isolation + luma + srcIn)
  virtual void pushMask(const std::optional<Box2d>& maskBounds) = 0;
  virtual void transitionMaskToContent() = 0;
  virtual void popMask() = 0;

  // Pattern tile recording (stacked for nested patterns)
  virtual void beginPatternTile(const Box2d& tileRect,
                                const Transform2d& patternToTarget) = 0;
  virtual void endPatternTile(bool forStroke) = 0;

  // Paint state
  virtual void setPaint(const PaintParams& paint) = 0;

  // Primitive drawing
  virtual void drawPath(const PathShape& path, const StrokeParams& stroke) = 0;
  virtual void drawRect(const Box2d& rect, const StrokeParams& stroke) = 0;
  virtual void drawEllipse(const Box2d& bounds, const StrokeParams& stroke) = 0;
  virtual void drawImage(const ImageResource& image, const ImageParams& params) = 0;
  virtual void drawText(const components::ComputedTextComponent& text,
                        const TextParams& params) = 0;

  virtual RendererBitmap takeSnapshot() const = 0;
};
```

Neutral structs (`RenderViewport`, `ResolvedClip`, `PaintParams`, `PathShape`, `StrokeParams`,
`ImageParams`, `TextParams`, `RendererBitmap`) wrap existing Donner math primitives and resolved
style data. Backends translate these to their own representations.

### RendererDriver

Located at `donner/svg/renderer/RendererDriver.h`. Backend-agnostic traversal of the flat render
tree:

- Uses `setTransform()` for absolute entity transforms (from `worldFromEntityTransform`) and
  `pushTransform()`/`popTransform()` only for local relative transforms (e.g., image
  `preserveAspectRatio`).
- Manages deferred layer pops via `DeferredPop` tracking: viewport clips, isolated layers, filter
  layers, entity clips, and masks are saved at subtree entry and restored when
  `subtreeInfo.lastRenderedEntity` is reached.
- Handles markers by re-traversing subtrees at each path vertex via `drawMarkers()`/
  `traverseRange()`, composing a per-vertex `surfaceFromCanvasTransform_`.
- Handles masks via `renderMask()`, traversing the mask shadow subtree between
  `pushMask()`/`transitionMaskToContent()`/`popMask()` calls.
- Handles patterns via `renderPattern()`, traversing pattern shadow subtrees between
  `beginPatternTile()`/`endPatternTile()`. `traverseRange()` also pre-renders nested patterns
  before drawing.

### Full-Skia renderer (shipped, later removed)

Implements `RendererInterface` using Skia. Owns canvas, bitmap, and font manager. Key
implementation details:

- Gradient resolution via `makeFillPaint()`/`makeStrokePaint()` with edge-case handling for
  single-stop gradients, zero-radius radials, and degenerate objectBoundingBox (1e-6 tolerance).
- 3-layer `saveLayer` mask technique: isolation + luma color filter + `kSrcIn` blend.
- Stacked `PatternState` for nested pattern recording (each level owns its own
  `SkPictureRecorder`).
- Clip path boolean operations via Skia `Op()` with layered intersection/union and
  `parentFromEntity` transform on layer pop.

### RendererTinySkia (shipped, parity incomplete)

Implements `RendererInterface` using `tiny-skia-cpp`. Lightweight alternative (~2MB vs ~50MB for
Skia) suitable for applications that do not need text shaping or advanced font rendering:

- Software rasterization only (no GPU acceleration).
- Supports path, rect, ellipse, image, gradient, clip, opacity-layer, mask, and pattern rendering.
- Exposes `takeSnapshot()` and PNG output for golden-based tests and tooling.
- Emits verbose warnings for unsupported text and filter operations rather than silently crashing.
- Does not currently provide text rendering.
- Does not currently provide full filter-effect parity.

### RendererRecorder (planned)

Wraps any `RendererInterface` using the tee pattern:

- Records every interface call to an in-memory list of `DrawCall` variant structs (using
  `std::variant` over per-method structs like `DrawCallBeginFrame`, `DrawCallSetTransform`,
  `DrawCallDrawPath`, etc.).
- Forwards all calls to the wrapped backend, so live rendering proceeds normally.
- Provides `toText()` for human-readable output of the recorded draw-call stream, with
  indentation tracking for push/pop nesting depth. Useful for debugging and test diagnostics.
- Provides `replay(RendererInterface&)` to iterate recorded calls and invoke them on any
  backend, enabling round-trip validation and backend comparison.
- No file serialization in the initial scope; the recording lives in memory for the duration of
  the frame. File-based serialization (protobuf/FlatBuffers) is future work if needed for
  offline replay tooling.

### MockRendererInterface (planned)

GMock-based backend enabling structural rendering assertions:

```cpp
TEST(RendererStructuralTest, SimpleRect) {
  auto svg = parseSVG(R"(<svg width="100" height="100">
    <rect x="10" y="10" width="80" height="80" fill="red"/>
  </svg>)");

  MockRendererInterface mock;
  InSequence seq;
  EXPECT_CALL(mock, beginFrame(_));
  EXPECT_CALL(mock, setPaint(HasFill(SolidColor(255, 0, 0, 255))));
  EXPECT_CALL(mock, drawRect(BoxEq(10, 10, 90, 90), _));
  EXPECT_CALL(mock, endFrame());

  RendererDriver driver(mock);
  driver.draw(svg);
}
```

This enables a new class of tests that verify rendering *structure* (what gets drawn, in what
order, with what paint) without pixel comparison, making tests fast, deterministic, and easy to
write.

### Backend Selection {#backend-selection}

#### Bazel

A `string_flag` controls which backend the `:renderer` target depends on:

```python
# donner/svg/renderer/BUILD.bazel
string_flag(
    name = "renderer_backend",
    build_setting_default = "tiny_skia",
    visibility = ["//visibility:public"],
)

donner_cc_library(
    name = "renderer",
    srcs = ["Renderer.cc"] + select({
        ":renderer_backend_tiny_skia": ["RendererTinySkiaBackend.cc"],
        "//conditions:default": ["LegacyFullSkiaBackend.cc"],
    }),
    hdrs = ["Renderer.h"],
    deps = [
        ":renderer_image_io",
        ":renderer_interface",
    ] + select({
        ":renderer_backend_tiny_skia": [":renderer_tiny_skia"],
        "//conditions:default": [":legacy_full_skia_renderer"],
    }),
)
```

The repository also defines `.bazelrc` aliases:

```sh
bazel test //...                     # default (tiny-skia)
bazel test <legacy full-Skia config> //...  # former full-Skia backend
bazel test --config=tiny-skia //...  # explicit tiny-skia
```

In Phase 2a, Bazel test targets also use `target_compatible_with` to exclude Skia-only suites such
as ASCII snapshot tests from `--config=tiny-skia`. Shared image-comparison suites use a selected
test backend shim and runtime feature checks so they still compile exactly one renderer backend per
build.

#### CMake (shipped)

CMake backend selection uses a cache variable matching Bazel's build-time selection model:

```sh
# Default: TinySkia backend
cmake -S . -B build
cmake -S . -B build -DDONNER_RENDERER_BACKEND=tiny_skia

# Skia backend
cmake -S . -B build -DDONNER_RENDERER_BACKEND=skia
```

The `DONNER_RENDERER_BACKEND` variable controls:
- Which backend library is fetched (Skia via FetchContent or tiny-skia-cpp as a subdirectory).
- Which backend-specific targets (`legacy_full_skia_renderer` or `renderer_tiny_skia`) are defined.
- Which backend source (`LegacyFullSkiaBackend.cc` or `RendererTinySkiaBackend.cc`) is compiled
  into the `renderer` target.

The generator (`tools/cmake/gen_cmakelists.py`) wraps backend-specific targets in
`if(DONNER_RENDERER_BACKEND ...)` guards and strips backend-specific sources/deps from the
shared `renderer` target, adding them conditionally via `target_sources()` and
`target_link_libraries()`.

CMake presets are provided for convenience:

```sh
cmake --preset default      # TinySkia (default)
cmake --preset skia         # Skia
cmake --preset tiny-skia    # TinySkia (explicit)
```

**Downstream override:** The `DONNER_RENDERER_BACKEND` variable uses `CACHE STRING`, so parent
projects can set it before including Donner via `add_subdirectory()` or `FetchContent` and the
default will not overwrite it:

```cmake
set(DONNER_RENDERER_BACKEND "skia" CACHE STRING "")
add_subdirectory(donner)
```

## Current Test Architecture

Phase 2a changed renderer testing to build only one backend at a time:

- `//donner/svg/renderer/tests:renderer_tests` renders the active backend and compares against
  shared checked-in goldens.
- `//donner/svg/renderer/tests:resvg_test_suite` now depends on the same image-comparison fixture
  and can render whichever backend Bazel selects.
- `ImageComparisonParams` declares backend restrictions and required backend features such as
  `Text`, `FilterEffects`, `AsciiSnapshot`, and `SkpDebug`.
- `renderer_test_backend` provides the active backend name, feature support, snapshot rendering,
  and optional Skia `.skp` capture without forcing both concrete backends into one test binary.
- Direct TinySkia-vs-Skia pixel comparison is removed from normal tests because it forced both
  backends to build and undermined the `bazel test //...` build-time goal.
- Skia-only ASCII snapshot tests now live in separate Bazel targets:
  `//donner/svg/renderer/tests:renderer_ascii_tests` and
  `//donner/svg/tests:svg_renderer_ascii_tests`.

## Testing and Validation

### Phase 1 (shipped)
- **Mock interaction tests:** `//donner/svg/renderer/tests:renderer_driver_tests` validates
  driver-to-interface call sequences for transforms, clips, opacity, text, images, masks,
  patterns, markers, and filters.
- **Image comparison tests:** `//donner/svg/renderer/tests:renderer_tests` (33 tests) and
  `//donner/svg/renderer/tests:resvg_test_suite` (660 tests) verify pixel-level correctness.
- **Golden images:** Ghostscript Tiger and donner splash are golden-tested.

### Phase 2a (shipped)
- **Backend-selected image comparison:** `renderer_tests` renders only the active backend selected
  by Bazel and compares against shared goldens.
- **Feature-gated skips:** tests that depend on text or filter support declare this through
  `ImageComparisonParams` instead of hard-coding dual-backend typed tests.
- **Skia-only ASCII coverage:** `renderer_ascii_tests` and `svg_renderer_ascii_tests` are marked
  incompatible with TinySkia builds.
- **TinySkia config validation:** `bazel test --config=tiny-skia`
  `//donner/svg/renderer/tests:renderer_tests` and `bazel test --config=tiny-skia`
  `//donner/svg/tests/...` verify that TinySkia builds and that Skia-only targets are skipped
  rather than compiled.

### Phase 2b (planned)
- Run the full resvg test suite against `RendererTinySkia` and document parity gaps.
- Add targeted golden-backed TinySkia regression cases for remaining edge cases as needed.

### Phase 3 (planned)
- Round-trip tests: render with Skia, record, replay against Skia, compare bitmaps.
- Text output golden tests: render representative SVGs, compare `toText()` output against
  expected call sequences.
- Verify tee pattern: recording backend produces identical pixels to direct rendering.

### Phase 4 (planned)
- Structural assertion tests for representative SVG features.
- Port existing mock interaction tests to use the `RenderAndExpect` fixture pattern.
- Ensure GMock backend tests run in <1s (no pixel rendering overhead).

## Dependencies

- **Skia** (existing): Full-featured 2D graphics library and current default backend.
- **tiny-skia-cpp** (shipped): Lightweight C++ 2D rasterizer at
  `github.com/jwmcglynn/tiny-skia-cpp`. ~200kb binary size. Added as a git subtree under
  `third_party/tiny-skia-cpp`.
- Phase 3 recording backend uses in-memory `std::variant` storage with no external
  serialization dependency. File-based serialization (protobuf/FlatBuffers) is future work.

## Alternatives Considered

- **Runtime backend switching via factory function:** Rejected in favor of build-time selection
  to avoid vtable overhead on every draw call in hot paths and to allow link-time dead-code
  elimination of unused backends.
- **Direct backend-vs-backend parity tests in normal suites:** Rejected because they force both
  concrete backends to compile into one test binary and make `bazel test //...` pay the Skia build
  cost even when validating TinySkia.
- **Single full-Skia renderer with compile-time `#ifdef` for tiny-skia:** Rejected because it would
  pollute the Skia backend with conditional compilation and prevent clean separation of concerns.
- **Protobuf/FlatBuffers recording format:** Deferred; in-memory recording with text output
  covers the immediate debugging and testing use cases without adding a serialization
  dependency. Can be revisited if offline replay tooling is needed.

## Open Questions

- Bazel downstream override uses `donner.configure(renderer = "skia")` in MODULE.bazel (planned,
  requires `config/extensions.bzl`). CMake override uses `set(DONNER_RENDERER_BACKEND "skia")`
  before including Donner, or CMake presets.
- How should resource lifetimes (fonts, images) be managed across backends--via shared caches in
  the driver or per-backend ownership?
- If file-based recording is added later, should the format be versioned independently?
- What is the text rendering story for `RendererTinySkia`? Options: (a) stub that renders
  bounding boxes, (b) integration with a lightweight shaping library like SheenBidi +
  stb_truetype, (c) require Skia backend for text-heavy SVGs.

## Future Work

- [ ] File-based recording serialization (protobuf/FlatBuffers) for offline replay tooling.
- [ ] GPU-accelerated backend (Vulkan/Metal/WebGPU) using the same `RendererInterface`.
- [ ] Streaming renderer that emits draw calls over IPC for out-of-process rendering.
- [ ] Animation timeline integration with the recording backend for frame-by-frame capture.
- [ ] Backend performance benchmarks comparing Skia vs tiny-skia for the resvg test corpus.
- [ ] Add Bazel module extension (`config/extensions.bzl`) for downstream `donner.configure()` override.
