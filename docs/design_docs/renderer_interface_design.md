# Design: Renderer Interface and Multi-Backend Support

**Status:** In Progress (Phase 1 shipped, Phase 2-4 planned)
**Author:** Claude Opus 4.6
**Created:** 2026-03-05

## Summary

The renderer interface decouples SVG document traversal from backend-specific drawing, enabling
multiple rendering backends behind a consistent API. Phase 1 (shipped) extracted a
backend-agnostic `RendererDriver` and `RendererInterface` from the monolithic `RendererSkia`,
achieving full resvg test suite parity. Subsequent phases add a lightweight `tiny-skia-cpp`
backend, a recording backend for draw-call serialization and replay, a GMock-integrated backend
for structural rendering assertions, and a Bazel/CMake backend selection mechanism for downstream
consumers.

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

- Land the tiny-skia-cpp subtree and create `RendererTinySkia` implementing `RendererInterface`.
- Add Bazel `string_flag` and CMake `option()` for backend selection.
- Implement `RendererRecorder` with protobuf-based serialization.

## Implementation Plan

### Phase 1: Interface extraction and Skia backend (shipped)
- [x] Add `RendererInterface.h` with frame control, state stack, paint, primitive, compositing,
  mask, pattern, and snapshot APIs.
- [x] Extract traversal code into `RendererDriver` with deferred layer management.
- [x] Implement all driver features: masks, patterns (with nesting), markers, filters, deferred
  pops, `subtreeConsumedBySubRendering`.
- [x] Adapt `RendererSkia` to implement `RendererInterface` with full feature parity.
- [x] Wire entry points (`SVGRenderer`, tooling, viewer, tests) through `RendererDriver`.
- [x] Add mock-based driver interaction tests (`renderer_driver_tests`).
- [x] Achieve full resvg test suite parity (660 tests, 0 regressions).

### Phase 2: tiny-skia-cpp backend
- [ ] Land `third_party/tiny-skia-cpp` subtree and add Bazel `cc_library` target.
- [ ] Create `RendererTinySkia` implementing `RendererInterface`.
  - [ ] Implement frame control: allocate pixel buffer, clear background.
  - [ ] Implement transform stack using `tiny_skia::Transform`.
  - [ ] Implement clip stack (rect clips and path clips via `tiny_skia::ClipPath`).
  - [ ] Implement `drawPath`/`drawRect`/`drawEllipse` with fill and stroke.
  - [ ] Implement gradient resolution (linear, radial) using `tiny_skia::Shader`.
  - [ ] Implement `pushIsolatedLayer`/`popIsolatedLayer` for opacity compositing.
  - [ ] Implement pattern tile recording and shader creation.
  - [ ] Implement mask compositing (3-layer technique adapted to tiny-skia API).
  - [ ] Implement filter layers (Gaussian blur via manual convolution or tiny-skia primitives).
  - [ ] Implement `drawImage` and `drawText` (text may require an external shaping library).
  - [ ] Implement `takeSnapshot` returning `RendererBitmap` from the pixel buffer.
- [ ] Add Bazel backend selection flag and CMake option
  (see [Backend Selection](#backend-selection)).
- [ ] Run resvg test suite against `RendererTinySkia` and document parity gaps.
- [ ] Add tiny-skia-specific smoke tests.

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

Donner's renderer was originally a monolithic `RendererSkia::Impl` class that interleaved
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
          RendererSkia  RendererTinySkia  Recorder  MockRenderer
```

### RendererInterface

Located at `donner/svg/renderer/RendererInterface.h`. The interface is Skia-free and uses only
Donner primitives:

```cpp
class RendererInterface {
 public:
  virtual ~RendererInterface() = default;

  // Frame lifecycle
  virtual void beginFrame(const RenderViewport& viewport) = 0;
  virtual void endFrame() = 0;

  // Transform stack: setTransform replaces the matrix (absolute entity transforms),
  // push/popTransform use save/concat/restore (local relative transforms).
  virtual void setTransform(const Transformd& transform) = 0;
  virtual void pushTransform(const Transformd& transform) = 0;
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
  virtual void pushMask(const std::optional<Boxd>& maskBounds) = 0;
  virtual void transitionMaskToContent() = 0;
  virtual void popMask() = 0;

  // Pattern tile recording (stacked for nested patterns)
  virtual void beginPatternTile(const Boxd& tileRect,
                                const Transformd& patternToTarget) = 0;
  virtual void endPatternTile(bool forStroke) = 0;

  // Paint state
  virtual void setPaint(const PaintParams& paint) = 0;

  // Primitive drawing
  virtual void drawPath(const PathShape& path, const StrokeParams& stroke) = 0;
  virtual void drawRect(const Boxd& rect, const StrokeParams& stroke) = 0;
  virtual void drawEllipse(const Boxd& bounds, const StrokeParams& stroke) = 0;
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

- Uses `setTransform()` for absolute entity transforms (from `entityFromWorldTransform`) and
  `pushTransform()`/`popTransform()` only for local relative transforms (e.g., image
  `preserveAspectRatio`).
- Manages deferred layer pops via `DeferredPop` tracking: viewport clips, isolated layers, filter
  layers, entity clips, and masks are saved at subtree entry and restored when
  `subtreeInfo.lastRenderedEntity` is reached.
- Handles markers by re-traversing subtrees at each path vertex via `drawMarkers()`/
  `traverseRange()`, composing a per-vertex `layerBaseTransform_`.
- Handles masks via `renderMask()`, traversing the mask shadow subtree between
  `pushMask()`/`transitionMaskToContent()`/`popMask()` calls.
- Handles patterns via `renderPattern()`, traversing pattern shadow subtrees between
  `beginPatternTile()`/`endPatternTile()`. `traverseRange()` also pre-renders nested patterns
  before drawing.

### RendererSkia (shipped)

Implements `RendererInterface` using Skia. Owns canvas, bitmap, and font manager. Key
implementation details:

- Gradient resolution via `makeFillPaint()`/`makeStrokePaint()` with edge-case handling for
  single-stop gradients, zero-radius radials, and degenerate objectBoundingBox (1e-6 tolerance).
- 3-layer `saveLayer` mask technique: isolation + luma color filter + `kSrcIn` blend.
- Stacked `PatternState` for nested pattern recording (each level owns its own
  `SkPictureRecorder`).
- Clip path boolean operations via Skia `Op()` with layered intersection/union and
  `entityFromParent` transform on layer pop.

### RendererTinySkia (planned)

Implements `RendererInterface` using `tiny-skia-cpp`. Lightweight alternative (~2MB vs ~50MB for
Skia) suitable for applications that do not need text shaping or advanced font rendering:

- Software rasterization only (no GPU acceleration).
- Path rendering via `tiny_skia::Pixmap` and `tiny_skia::Paint`.
- Gradient support via `tiny_skia::LinearGradient`/`tiny_skia::RadialGradient`.
- Pattern support via offscreen `Pixmap` rendering and tiled blitting.
- Mask compositing using manual pixel-level alpha operations.
- Text rendering is limited; consumers needing full text support should use the Skia backend.

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
    name = "rendering_backend",
    build_setting_default = "skia",
    values = ["skia", "tiny_skia"],
    visibility = ["//visibility:public"],
)

config_setting(name = "backend_skia",
    flag_values = {":rendering_backend": "skia"})
config_setting(name = "backend_tiny_skia",
    flag_values = {":rendering_backend": "tiny_skia"})

donner_cc_library(
    name = "renderer",
    visibility = ["//visibility:public"],
    deps = [
        ":renderer_driver",
        ":renderer_interface",
    ] + select({
        ":backend_skia": [":renderer_skia"],
        ":backend_tiny_skia": [":renderer_tiny_skia"],
    }),
)
```

Consumers select the backend with:
```sh
bazel build //my:target --@donner//donner/svg/renderer:rendering_backend=tiny_skia
```

#### CMake

A CMake `option()` controls the backend:

```cmake
set(DONNER_RENDERING_BACKEND "skia" CACHE STRING "Rendering backend (skia, tiny_skia)")
set_property(CACHE DONNER_RENDERING_BACKEND PROPERTY STRINGS skia tiny_skia)

if(DONNER_RENDERING_BACKEND STREQUAL "skia")
  target_link_libraries(donner_renderer PUBLIC donner_renderer_skia)
elseif(DONNER_RENDERING_BACKEND STREQUAL "tiny_skia")
  target_link_libraries(donner_renderer PUBLIC donner_renderer_tiny_skia)
endif()
```

Both backends export the same public headers (`RendererInterface.h`, `RendererDriver.h`) and
a concrete renderer class with a `draw(SVGDocument&)` entry point, so consumer code does not
need conditional compilation.

## Testing and Validation

### Phase 1 (shipped)
- **Mock interaction tests:** `//donner/svg/renderer/tests:renderer_driver_tests` validates
  driver-to-interface call sequences for transforms, clips, opacity, text, images, masks,
  patterns, markers, and filters.
- **Image comparison tests:** `//donner/svg/renderer/tests:renderer_tests` (33 tests) and
  `//donner/svg/renderer/tests:resvg_test_suite` (660 tests) verify pixel-level correctness.
- **Golden images:** Ghostscript Tiger and donner splash are golden-tested.

### Phase 2 (planned)
- Run the full resvg test suite against `RendererTinySkia` and document parity gaps.
- Add tiny-skia-specific smoke tests for gradient, pattern, mask, and filter edge cases.
- Compare pixel output between Skia and tiny-skia for a representative subset.

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

- **Skia** (existing): Full-featured 2D graphics library. ~50MB binary size contribution.
- **tiny-skia-cpp** (planned): Lightweight C++ 2D rasterizer at
  `github.com/jwmcglynn/tiny-skia-cpp`. ~2MB binary size. Added as a git subtree under
  `third_party/tiny-skia-cpp`.
- Phase 3 recording backend uses in-memory `std::variant` storage with no external
  serialization dependency. File-based serialization (protobuf/FlatBuffers) is future work.

## Alternatives Considered

- **Runtime backend switching via factory function:** Rejected in favor of build-time selection
  to avoid vtable overhead on every draw call in hot paths and to allow link-time dead-code
  elimination of unused backends.
- **Single `RendererSkia` with compile-time `#ifdef` for tiny-skia:** Rejected because it would
  pollute the Skia backend with conditional compilation and prevent clean separation of concerns.
- **Protobuf/FlatBuffers recording format:** Deferred; in-memory recording with text output
  covers the immediate debugging and testing use cases without adding a serialization
  dependency. Can be revisited if offline replay tooling is needed.

## Open Questions

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
