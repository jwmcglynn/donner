# Design: SVG Filter Primitives

**Status:** Complete, gaps identified
**Author:** Claude Opus 4.6
**Created:** 2026-04-01

## Summary

The filter module (`src/tiny_skia/filter/`) implements SVG2 filter effects as a
standalone subsystem. It provides 17 filter primitive types, a graph executor
(`FilterGraph`) for composing multi-step filter chains, and both uint8 (sRGB)
and float (linear RGB) processing paths with SIMD acceleration.

This is **new functionality** not present in the upstream Rust tiny-skia. It
was written for the C++ port to support SVG `<filter>` rendering in donner.

## What Shipped

### Primitives (17 types)

| Primitive | SVG element | Implementation |
|-----------|-------------|----------------|
| GaussianBlur | `feGaussianBlur` | Discrete kernel (σ<2) / 3-pass box blur (σ≥2) |
| Flood | `feFlood` | Solid color fill |
| Offset | `feOffset` | Integer pixel translation |
| Composite | `feComposite` | Porter-Duff + arithmetic mode |
| Blend | `feBlend` | All 16 CSS blend modes |
| Merge | `feMerge` | N-input layer compositing |
| ColorMatrix | `feColorMatrix` | 5×4 matrix + saturate/hueRotate/luminanceToAlpha |
| ComponentTransfer | `feComponentTransfer` | Identity/Table/Discrete/Linear/Gamma per channel |
| ConvolveMatrix | `feConvolveMatrix` | Arbitrary kernel with edge modes |
| Morphology | `feMorphology` | Erode/Dilate with independent X/Y radii |
| Tile | `feTile` | Region tiling |
| Turbulence | `feTurbulence` | Perlin noise (fractalNoise + turbulence) |
| DisplacementMap | `feDisplacementMap` | Channel-based displacement |
| DiffuseLighting | `feDiffuseLighting` | Lambertian with point/distant/spot lights |
| SpecularLighting | `feSpecularLighting` | Phong with point/distant/spot lights |
| DropShadow | `feDropShadow` | Composite primitive (flood+offset+blur+merge) |
| Image | `feImage` | Pre-loaded pixel data injection (see Design Decisions) |

### Architecture

- **Dual bit-depth**: Every primitive has both `Pixmap` (uint8 sRGB) and
  `FloatPixmap` (float [0,1] linear) overloads. The graph executor handles
  sRGB↔linear conversion automatically based on `useLinearRGB`.
- **Graph executor**: `executeFilterGraph()` processes nodes in order, managing
  named intermediate results, standard inputs (SourceGraphic, SourceAlpha,
  FillPaint, StrokePaint), and per-node subregion clipping.
- **SIMD acceleration**: `FloatPixmap` uses NEON intrinsics on ARM64 for
  uint8↔float conversion. `SimdVec.h` provides portable SIMD helpers for
  filter inner loops.
- **Rotation-aware clipping**: Supports per-pixel point-in-rect testing via
  `userSpaceSubregion` + `filterFromDevice` affine transform for non-axis-aligned
  filter regions.

### Build Integration

- **Bazel**: Opt-in. The filter module is a separate target
  (`//src/tiny_skia/filter:filter`) not included in the default `tiny_skia_lib`.
  Consumers add it explicitly via `deps`.
- **CMake**: Separate library targets. Filters are built as `tiny_skia_filter`
  (native SIMD) and `tiny_skia_filter_scalar` (portable), linked against
  their respective base libraries. Consumers add filters via
  `target_link_libraries(your_target PRIVATE tiny_skia_filter)`.

### Goals

1. **Clean API boundary** — filters operate on `Pixmap`/`FloatPixmap` without
   coupling to the rasterization pipeline. No changes to core types were needed.
2. **SVG2 spec coverage** — all SVG2 filter primitives are implemented, including
   `feDropShadow` from the CSS Filter Effects spec.
3. **Performance benchmarks** — `FilterPerfBench.cpp` covers all primitives with
   pixels/second metrics and a regression test script.
4. **No TODOs/FIXMEs left** — code is clean of development markers.

## What Needs Work (Production Readiness)

### P0 — Must Fix

1. **No correctness tests.** The filter module has performance benchmarks but
   zero unit or integration tests that verify output correctness. A filter
   could produce completely wrong output and no test would catch it. Need:
   - Golden-image tests for each primitive (uint8 and float paths).
   - Edge-case tests: zero-size inputs, sigma=0 blur, empty graph, single-node
     graph, named result resolution, subregion clipping.
   - Round-trip tests: sRGB→linear→sRGB color space conversion accuracy.

### P1 — Should Fix

2. **No `FilterGraph` integration test exercising multi-node chains.** The
   benchmarks test individual primitives but not the graph executor's named
   result resolution, subregion clipping, or color space conversion logic.

3. **Allocation limits.** Large blur radii or high-resolution turbulence could
   allocate significant memory. Consider documenting or enforcing caps.

4. **Error reporting.** `executeFilterGraph` returns `bool` but doesn't
   indicate *which* node failed or *why*. For debugging, a richer error
   type or callback would help.

### P2 — Nice to Have

5. **Thread safety documentation.** Filter primitives appear stateless and
   thread-safe, but this is not documented in headers.

6. **Float path SIMD on x86.** `FloatPixmap` only uses NEON; x86 AVX2
   could accelerate float↔uint8 conversion similarly.

## Implementation Plan

### Phase 1: Correctness Tests (P0)

Each step should leave `bazel test //...` green.

- [ ] **IP-01**: Create `tests/integration/FilterTest.cpp` with a Bazel test
  target (both native and scalar modes). Start with a test fixture that creates
  a known source pixmap (e.g., 64×64 solid color, 64×64 gradient).
- [ ] **IP-02**: Add golden-image tests for each standalone primitive (uint8):
  GaussianBlur, Flood, Offset, Blend, Composite, Merge, ColorMatrix,
  ComponentTransfer, ConvolveMatrix, Morphology, Tile, Turbulence,
  DisplacementMap, DiffuseLighting, SpecularLighting.
- [ ] **IP-03**: Add golden-image tests for float-path variants of the same
  primitives, verifying that float and uint8 paths produce visually equivalent
  results (within tolerance).
- [ ] **IP-04**: Edge-case tests: zero-size pixmap, sigma=0 blur, empty
  FilterGraph, single-node graph, 0-radius morphology, identity color matrix,
  empty merge input list.
- [ ] **IP-05**: Color space round-trip test: convert uint8 sRGB → float
  linear → uint8 sRGB and verify max error ≤ 1 LSB.

### Phase 2: FilterGraph Integration Tests (P1)

- [ ] **IP-06**: Multi-node graph test: build a 3-node chain
  (blur → offset → composite over source) and verify output against
  manually-composed standalone primitive calls.
- [ ] **IP-07**: Named result resolution test: graph with `result` attributes
  and `Named` input references, verify correct intermediate routing.
- [ ] **IP-08**: Subregion clipping test: verify that node subregions correctly
  clip primitive output and that pixels outside the subregion are untouched.
- [ ] **IP-09**: Color space conversion test: graph with `useLinearRGB = true`,
  verify that sRGB↔linear conversion happens around each node.
- [ ] **IP-10**: Standard inputs test: graph using FillPaint and StrokePaint
  standard inputs, verify they are correctly injected.

### Phase 3: Hardening (P1)

- [ ] **IP-11**: Document allocation behavior in `FilterGraph.h` — note that
  each intermediate result allocates a full pixmap, and that large blur radii
  scale kernel allocation with sigma.
- [ ] **IP-12**: Add `@note` thread-safety documentation to filter headers
  stating that primitives are stateless and safe to call concurrently on
  independent pixmaps.

## Lessons Learned

- **Benchmark ≠ test.** Having 40+ benchmark cases created a false sense of
  coverage. Benchmarks verify performance characteristics and that code doesn't
  crash, but don't verify correctness. Correctness tests should have been
  written alongside the implementation.

- **CMake/Bazel parity matters.** The Bazel build correctly makes filters
  opt-in. CMake now matches with separate `tiny_skia_filter` /
  `tiny_skia_filter_scalar` library targets.

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| Dual uint8/float paths | SVG spec requires linear-light processing; uint8 path avoids conversion cost when not needed |
| Graph-based executor | SVG filter chains are DAGs with named intermediates; a graph naturally maps to this |
| Standalone module | Filters don't need rasterization internals; clean boundary reduces coupling |
| No upstream Rust equivalent | This is original C++ code, not a port — different quality bar applies |
| `feImage` requires pre-loaded pixels | The library has no I/O or network layer. The `graph_primitive::Image` struct accepts raw RGBA pixel data and dimensions — the caller is responsible for loading images from URIs, files, or other sources. This keeps the filter module dependency-free and avoids pulling in image codec or HTTP dependencies. |
