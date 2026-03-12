# Filter Performance Optimization

**Status:** Design
**Author:** jwm
**Date:** 2025-03-11
**Related:** [filter_effects.md](filter_effects.md), [renderer_interface_design.md](renderer_interface_design.md)

## Problem

Filter rendering performance has regressed significantly, particularly Gaussian blur. Two root causes:

1. **Skia backend falls through to CPU fallback far too often.** The `getSimpleNativeSkiaBlur()`
   eligibility check has an inverted condition (line 168 of `RendererSkia.cc`) that rejects
   *isotropic* blurs — the most common case. The Donner Splash SVG has three single-node isotropic
   Gaussian blurs (`stdDeviation="6"`, `"3"`, `"4.5"`) that all hit the slow CPU fallback instead
   of Skia's GPU-accelerated `SkImageFilters::Blur()`. Beyond the bug fix, only single-node
   `feGaussianBlur` is eligible for native Skia at all; every other filter graph topology falls
   through entirely.

2. **The CPU fallback (tiny-skia) is unoptimized.** The Gaussian blur implementation is a naive
   per-pixel, per-channel scalar loop with no SIMD vectorization, no cache-friendly memory access
   patterns, and no reuse of intermediate allocations. For large images (e.g. 1200×900 at 2×
   density), this dominates frame time.

## Goals

- **Skia backend:** Eliminate the CPU fallback entirely. Lower all 17 filter primitives to native
  `SkImageFilter` chains — Skia has APIs for every one of them.
- **TinySkia backend:** Achieve performance within **2× of the Skia backend** for equivalent
  workloads. Currently 70× slower (14s vs 200ms for Donner Splash). Target: ~400ms or less.
- Maintain pixel-level correctness — no regression in resvg test suite thresholds.

## Non-Goals

- GPU compute shaders or custom GPU filter implementations.
- Multithreaded filter execution (potential future work, orthogonal to this design).
- Changing the `RendererInterface` API or filter graph data model.
- Exact bit-level match between Skia and TinySkia outputs (minor quantization differences are
  acceptable as long as resvg thresholds hold).

---

## Part 1: Native Skia Lowering

### 1.1 Bug Fix: Inverted Isotropic Blur Check

**File:** `donner/svg/renderer/RendererSkia.cc:168`

```cpp
// CURRENT (buggy): rejects when X ≈ Y (isotropic — the common case!)
NearEquals(blur->stdDeviationX, blur->stdDeviationY, 1e-6)

// FIX: remove this check entirely. SkImageFilters::Blur() handles anisotropic
// blur natively (separate sigma per axis), so there's no reason to restrict
// to isotropic OR anisotropic.
```

This alone fixes the Donner Splash performance regression.

### 1.2 Relax Uniform-Scale Constraint

The current check (lines 172–179) rejects native Skia blur when the transform has non-uniform
scale or rotation. `SkImageFilters::Blur()` operates in the local coordinate system of the
`saveLayer`, so it produces correct results for uniform-scale + rotation but not for non-uniform
scale with rotation. However, the non-uniform-scale rejection is overly conservative:

- **Pure scale (no rotation/skew):** Safe for native Skia — sigma is applied per-axis in device
  space. Relax the `NearEquals(xLength, yLength)` constraint when there's no rotation.
- **Uniform scale + rotation:** Already handled correctly by Skia — keep as eligible.
- **Non-uniform scale + rotation/skew:** Keep as ineligible (sigma interaction with rotation is
  non-trivial).

### 1.3 Extend Native Lowering to Multi-Node Graphs

Currently only single-node `feGaussianBlur` is eligible. Skia's `SkImageFilter` API supports
composable filter chains. Extend lowering to cover the most common multi-node patterns:

#### Architecture: Full Graph Lowering

**Reference implementation:** Skia's own SVG module (`external/+_repo_rules3+skia/modules/svg/`)
uses the same pattern we'll implement:

1. **Sequential walk** through filter nodes in document order
2. **Named result registry** (`std::map<string, sk_sp<SkImageFilter>>`) for fan-out
3. **Previous result** tracking (implicit output of last node)
4. **Colorspace conversion** at node boundaries via `SkColorFilters::SRGBToLinearGamma()` /
   `LinearToSRGBGamma()`

Key insight from Skia's SVG source: `SkImageFilter` inputs are wired as constructor arguments,
forming a DAG. `nullptr` input = implicit source image (SourceGraphic in our model). The entire
graph is built as a single `SkImageFilter` tree passed to `saveLayer()`'s paint.

**Natively-lowerable primitives** (via `SkImageFilters`):

Skia has native `SkImageFilter` equivalents for **all 17 SVG filter primitives**:

| SVG Primitive          | Skia API                          | Notes                              |
|------------------------|-----------------------------------|------------------------------------|
| `feGaussianBlur`       | `SkImageFilters::Blur()`          | Already implemented for single-node |
| `feOffset`             | `SkImageFilters::Offset()`        | Direct mapping                     |
| `feFlood`              | `SkImageFilters::Shader()` + `SkShaders::Color()` | Solid color fill  |
| `feBlend`              | `SkImageFilters::Blend()`         | All blend modes supported          |
| `feComposite`          | `SkImageFilters::Arithmetic()` / `Blend()` | `arithmetic` has dedicated API |
| `feMerge`              | `SkImageFilters::Merge()`         | N-input merge                      |
| `feColorMatrix`        | `SkColorFilters::Matrix()` via `ColorFilter()` | Direct 5x4 matrix    |
| `feMorphology`         | `SkImageFilters::Dilate()` / `Erode()` | Direct mapping              |
| `feDropShadow`         | `SkImageFilters::DropShadow()`    | Direct mapping                     |
| `feTurbulence`         | `SkShaders::MakeTurbulence()` / `MakeFractalNoise()` via `Shader()` | Implements same SVG spec algorithm |
| `feConvolveMatrix`     | `SkImageFilters::MatrixConvolution()` | Has tileMode + convolveAlpha params |
| `feDisplacementMap`    | `SkImageFilters::DisplacementMap()` | Channel selectors + scale         |
| `feDiffuseLighting`    | `SkImageFilters::PointLitDiffuse()` / `DistantLitDiffuse()` / `SpotLitDiffuse()` | All 3 light types |
| `feSpecularLighting`   | `SkImageFilters::PointLitSpecular()` / `DistantLitSpecular()` / `SpotLitSpecular()` | All 3 light types |
| `feComponentTransfer`  | `SkColorFilters::TableARGB()` via `ColorFilter()` | Per-channel 256-entry LUT; covers table/discrete cases. Linear/gamma need pre-computation into LUT. |
| `feTile`               | `SkImageFilters::Tile()`          | src→dst rect tiling               |
| `feImage`              | `SkImageFilters::Image()`         | Image source with rect mapping     |

This means **every filter graph can in principle be fully lowered to Skia** — no CPU fallback
needed at all.

#### Implementation: `buildNativeSkiaFilterDAG()`

New function in `RendererSkia.cc` that replaces `getSimpleNativeSkiaBlur()`. Algorithm:

```
function buildNativeSkiaFilterDAG(filterGraph, deviceFromFilter) -> sk_sp<SkImageFilter>:
  previousFilter = nullptr  // nullptr = SourceGraphic (Skia convention)
  namedResults = {}         // map<string, sk_sp<SkImageFilter>>

  for each node in filterGraph.nodes:
    // 1. Resolve inputs
    inputs[] = for each node.input:
      Previous → previousFilter
      Named(name) → namedResults[name]
      SourceGraphic → nullptr
      SourceAlpha → ColorFilter(alpha-extract-matrix, nullptr)
      FillPaint/StrokePaint → Image(prerendered)

    // 2. Apply colorspace conversion if needed
    if node needs linearRGB and previous was sRGB:
      inputs[i] = ColorFilter(SRGBToLinearGamma(), inputs[i])

    // 3. Lower primitive to SkImageFilter
    filter = lowerPrimitive(node.primitive, inputs, cropRect)
    if filter == null: return null  // fallback to CPU

    // 4. Apply subregion clipping
    if node has subregion:
      filter = Crop(subregion, filter)

    // 5. Register result
    if node.result: namedResults[node.result] = filter
    previousFilter = filter

  // 6. Convert final result back to sRGB if needed
  if lastNodeWasLinearRGB:
    previousFilter = ColorFilter(LinearToSRGBGamma(), previousFilter)

  return previousFilter
```

#### Input Wiring Details (from Skia SVG source)

**Multi-input primitives** — argument order matters:
- `feBlend`: `Blend(mode, background=in1, foreground=in2)`
- `feComposite`: `Blend(mode, background=in1, foreground=in2)` or
  `Arithmetic(k1,k2,k3,k4, enforcePM, background=in1, foreground=in2)`
- `feDisplacementMap`: `DisplacementMap(xCh, yCh, scale, displacement=in2, color=in1)` —
  **note swapped order** vs SVG spec naming
- `feMerge`: `Merge(inputsArray, count)` — collects all merge node inputs

**SourceAlpha extraction** (from Skia SVG):
```cpp
// 5x4 color matrix: zero RGB, keep alpha
SkColorMatrix alphaMatrix;
alphaMatrix.setScale(0, 0, 0, 1.0f);
ColorFilter(SkColorFilters::Matrix(alphaMatrix), inputFilter)
```

**Colorspace conversion** (from Skia SVG):
```cpp
// sRGB → linearRGB: wrap input in SRGBToLinearGamma
ColorFilter(SkColorFilters::SRGBToLinearGamma(), filter)
// linearRGB → sRGB: wrap in LinearToSRGBGamma
ColorFilter(SkColorFilters::LinearToSRGBGamma(), filter)
```

#### Graph Topology Support

The `buildNativeSkiaFilterDAG()` function above handles all graph topologies:
- **Fan-out:** Named results stored in map, referenced by multiple downstream nodes.
- **Multi-input:** Resolved per-primitive (feComposite takes in1+in2 as constructor args).
- **Standard inputs:** `SourceGraphic` → `nullptr`, `SourceAlpha` → alpha-extract
  `ColorFilter`, `FillPaint`/`StrokePaint` → pre-rendered `SkImageFilters::Image()`.

This is exactly how Skia's own SVG module implements it (see `SkSVGFilterContext`).

### 1.4 Color Space Handling for Native Lowering

When `color-interpolation-filters: linearRGB`, Skia's filters operate in sRGB by default. Two
options:

- **Option A:** Wrap the filter chain in `SkColorFilters` for sRGB→linear conversion on input and
  linear→sRGB on output. This is the simplest approach and matches the current CPU path's behavior.
- **Option B:** Use `SkColorSpace::MakeSRGBLinear()` on the `SkSurface` used for the filter layer.
  This makes all Skia operations happen in linear space automatically.

Prefer Option B — it's more correct for complex chains and avoids per-primitive conversion overhead.

### 1.5 FilterRegion / Subregion Support

Current native path rejects any graph with a `filterRegion` (line 148). Skia's
`SkImageFilters::CropRect` directly supports subregion clipping. Extend the eligibility check to
accept `filterRegion` by wrapping the outermost filter in a crop rect, as already partially
implemented (lines 843–847).

---

## Part 2: tiny-skia CPU Optimization

The TinySkia backend must stand on its own as a viable renderer — not a "fallback" that's 70×
slower. Target: within **2× of the Skia backend** for equivalent workloads (Donner Splash: ~400ms
vs Skia's ~200ms). This requires a ~35× improvement from the current 14s baseline.

### 2.1 Gaussian Blur — Algorithmic Improvements

**Current implementation:** `third_party/tiny-skia-cpp/src/tiny_skia/filter/GaussianBlur.cpp`

The current blur is a direct convolution: for each pixel, for each channel, multiply-accumulate
across the kernel. This is O(w × h × kernelSize × 4 channels) per pass.

#### 2.1.1 Running-Sum Box Blur

For σ ≥ 2.0, the code already uses a box-kernel approximation (3–4 convolved box filters). Box
blurs are eligible for the **running sum** optimization:

```
// Instead of:
for each pixel x:
    sum = 0
    for each kernel tap k:
        sum += src[x + k]
    dst[x] = sum / width

// Use:
sum = initial_window_sum
for each pixel x:
    sum += src[x + radius]    // add entering pixel
    sum -= src[x - radius - 1] // remove leaving pixel
    dst[x] = sum / width
```

This reduces the horizontal pass from O(w × kernelSize) to O(w) per row — a massive speedup for
large sigma values. The same applies to the vertical pass.

**Implementation notes:**
- Edge mode handling complicates the running sum (need to resolve entering/leaving pixels).
  For `EdgeMode::None` (transparent black outside), this simplifies to conditional adds.
- For `EdgeMode::Duplicate`, clamp the entering/leaving indices.
- Each box pass already has its own kernel, so iterate the running sum 3–4 times.

#### 2.1.2 Process 4 Channels Together

The innermost loop currently iterates `for (int channel = 0; channel < 4; ++channel)`, loading
one byte at a time. Since RGBA is stored interleaved (4 bytes per pixel), process all 4 channels
simultaneously using wider accumulators:

```cpp
// Scalar 4-channel approach (no SIMD needed):
uint64_t sumRGBA[4] = {0, 0, 0, 0};
// or use a single __m128i for SIMD
```

This improves cache utilization (one load serves all 4 channels) and reduces loop overhead.

### 2.2 SIMD Vectorization

Target ARM NEON (Apple Silicon) with compile-time detection. x86 SSE2/AVX2 can be added later
with the same abstraction.

#### 2.2.1 SIMD Strategy

Use a thin abstraction layer to avoid duplicating scalar and SIMD paths:

```cpp
// tiny_skia/filter/SimdHelpers.h
namespace tiny_skia::filter::simd {

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
using f32x4 = float32x4_t;
using u8x16 = uint8x16_t;
// ... load, store, multiply-add, horizontal sum
#elif defined(__SSE2__)
using f32x4 = __m128;
// ...
#else
// Scalar fallback struct with same interface
#endif

}
```

#### 2.2.2 Target Operations for SIMD

**Priority 1 — Gaussian blur (horizontal pass):**
- Load 4 consecutive RGBA pixels (16 bytes) → process 4 pixels × 4 channels in one iteration.
- For box blur running sum: SIMD add/subtract of 4-channel pixel values.
- For weighted kernel: SIMD multiply-accumulate with broadcast weight.

**Priority 2 — FloatPixmap conversion:**
- `fromPixmap()`: Load 16 bytes → convert to 4 floats × 4 channels. NEON has `vcvtq_f32_u32`.
- `toPixmap()`: Convert 4 floats → 4 bytes with clamp and round. NEON has `vcvtnq_u32_f32`.
- Currently a scalar loop over every pixel — easy 4× speedup.

**Priority 3 — Color space conversion:**
- sRGB↔linear LUT lookups are inherently scalar, but the surrounding multiply/clamp operations
  benefit from SIMD.
- For float pipeline: the piecewise sRGB function (`s <= 0.04045 ? s/12.92 : pow(...)`) can use
  SIMD for the linear branch and scalar for the pow branch, with blending.

**Priority 4 — Per-pixel operations (blend, composite, color matrix):**
- These are simple per-pixel arithmetic — good SIMD candidates but typically not bottlenecks
  compared to blur.

### 2.3 Memory Access Patterns

#### 2.3.1 Vertical Pass Cache Optimization

The vertical convolution pass has poor cache locality: it reads column-wise through a row-major
buffer, causing a cache miss on every row. Two approaches:

- **Transpose-blur-transpose:** Transpose the image, apply horizontal blur (cache-friendly), then
  transpose back. Transposition is O(w×h) but cache-friendly with block tiling.
- **Tile-based processing:** Process the image in cache-friendly tiles (e.g., 64×64 pixels) for
  both passes.

The transpose approach is simpler and well-proven in image processing libraries.

#### 2.3.2 Allocation Reuse

Current code allocates fresh `std::vector` buffers for each blur pass:

```cpp
// GaussianBlur.cpp:221-222
std::vector<std::uint8_t> buffer(pixmap.data().begin(), pixmap.data().end());
std::vector<std::uint8_t> scratch(buffer.size());
```

For repeated filter application (e.g., animation), consider a thread-local or per-frame scratch
buffer pool. Lower priority since allocation is typically dwarfed by computation time for large
images.

#### 2.3.3 Aligned Allocations for SIMD

`FloatPixmap` and `Pixmap` use plain `std::vector` with no alignment guarantees. For SIMD loads
(`vld1q_f32` on NEON), 16-byte alignment is preferred. Options:

- Use `std::aligned_alloc` or a custom allocator for the backing storage.
- Alternatively, use unaligned SIMD loads (`vld1q_f32` on NEON handles unaligned, unlike older
  SSE). On Apple Silicon, unaligned NEON loads have no penalty, so this may be a non-issue.

### 2.4 Morphology Optimization

Current morphology (erode/dilate) is O(w × h × rx × ry). The van Herk/Gil-Werman algorithm
reduces this to O(w × h) regardless of radius, using a 3-pass sliding window with forward scan,
backward scan, and merge. This is noted in `filter_effects.md` as future work.

---

## Implementation Plan

### Phase 1: Quick Wins — Skia Native + Blur Algorithm

1. **Fix the inverted isotropic blur check** in `getSimpleNativeSkiaBlur()`.
   - One-line fix: remove the `NearEquals` condition on line 168.
   - Immediately fixes Donner Splash and all single-node isotropic blurs on Skia backend.

2. **Relax filterRegion rejection** — allow `filterRegion` with `CropRect` wrapping.

3. **Implement running-sum box blur** in `GaussianBlur.cpp` for the `numerators` (box kernel)
   path. Both uint8 and float variants.
   - Replaces O(kernelSize) inner loop with O(1) sliding window.
   - Biggest algorithmic improvement for σ ≥ 2.0. Est. ~10× for blur-dominated workloads.

4. **Process 4 channels together** in blur convolution — restructure the inner loop to load one
   RGBA pixel and accumulate all 4 channels simultaneously instead of per-channel iteration.

### Phase 2: Skia Full Native Lowering

Since Skia has native APIs for ALL 17 filter primitives, build a general-purpose graph lowering
engine that eliminates the CPU fallback entirely.

5. **Build `SkImageFilter` DAG from `FilterGraph`** — walk the donner filter graph and construct
   the equivalent Skia DAG bottom-up. Each `GraphNode` becomes an `SkImageFilter` node with its
   inputs wired as constructor arguments. This replaces the current single-node eligibility check
   with a general approach.

6. **Lower simple primitives first** (least risk):
   `feOffset`, `feFlood`, `feBlend`, `feMerge`, `feDropShadow`, `feColorMatrix`, `feMorphology`.

7. **Lower spatial/generative primitives**:
   `feTurbulence` (via `SkShaders::MakeTurbulence()`/`MakeFractalNoise()` + `Shader()`),
   `feDisplacementMap`, `feConvolveMatrix`, `feTile`, `feImage`.

8. **Lower lighting primitives**:
   `feDiffuseLighting`, `feSpecularLighting` — Skia has per-light-type factories
   (`PointLitDiffuse`, `DistantLitSpecular`, `SpotLitDiffuse`, etc.).

9. **Lower `feComponentTransfer`**: Pre-compute 256-entry LUT per channel from the transfer
   function, then use `SkColorFilters::TableARGB()`.

10. **Handle `feComposite` arithmetic mode** via `SkImageFilters::Arithmetic(k1, k2, k3, k4)`.

### Phase 3: SIMD + Cache Optimization (TinySkia parity)

These optimizations target the TinySkia backend to achieve within 2× of Skia performance.

11. **Add `SimdHelpers.h`** with NEON intrinsics and scalar fallback.
    - Start with: load/store 4 floats, load/store 16 bytes, multiply-accumulate, horizontal sum.

12. **SIMD-accelerate FloatPixmap conversion** (`fromPixmap` / `toPixmap`).
    - Simple batch operation, good first SIMD target.

13. **SIMD-accelerate horizontal blur** (float path).
    - 4-channel multiply-accumulate with broadcast kernel weight.

14. **Vertical pass cache optimization** — transpose-blur-transpose or tiled processing.
    - Eliminates column-wise cache misses that currently dominate the vertical pass.

15. **Reduce unnecessary FloatPixmap round-trips.** Currently every filter node converts uint8→
    float→uint8. For chains of operations, keep data in float and convert only at the boundary.
    (The filter graph executor already operates in float internally — audit whether the blur is
    being called through the uint8 entry point unnecessarily.)

### Phase 4: Polish

16. **van Herk/Gil-Werman morphology** — O(w×h) erode/dilate regardless of radius.

17. **Profile-guided threshold tuning** — after all optimizations, re-measure and adjust any
    resvg test thresholds that shifted due to algorithmic changes.

18. **Benchmark suite** — automated perf regression tests for Donner Splash and isolated
    primitives.

---

## Benchmarking

### Current Measurements

Donner Splash SVG (3 single-node isotropic Gaussian blurs: σ=3, 4.5, 6):

| Backend    | Time   | Notes                                          |
|------------|--------|-------------------------------------------------|
| Skia       | ~200ms | **But uses CPU fallback** due to isotropic bug  |
| TinySkia   | ~14s   | Pure CPU scalar blur                            |
| Skia native| ~?ms   | Expected after fixing isotropic check           |

**70× gap between Skia (CPU fallback) and TinySkia.** Even the Skia path is slower than it should
be — the 200ms is Skia doing the same CPU blur that TinySkia does, but through Skia's own
rasterizer (which has better-optimized blur kernels). With the isotropic fix, Skia will use
`SkImageFilters::Blur()` which is GPU-capable and should drop to single-digit milliseconds.

The 14s TinySkia time is explained by the algorithmic complexity. For the Donner Splash at ~1200×900
viewport, each blur filter operates on a full-frame pixmap (1,080,000 pixels). With σ=6, the box
blur approximation uses window ≈ 11, convolved 3× to produce an effective kernel width of ~33 taps.
Per blur filter: 1.08M pixels × 4 channels × 33 taps × 2 passes (H+V) = ~285M multiply-accumulates,
plus the 3-pass box convolution overhead. Three blur filters = ~1B operations, all scalar with poor
cache locality on the vertical pass. The `float` pipeline adds FloatPixmap conversion overhead
(two full-frame copies per filter) and sRGB↔linearRGB conversion.

### Target Performance

Goal: TinySkia within **2× of Skia** for equivalent CPU workloads.

| Backend    | Target  | Speedup | How                                        |
|------------|---------|---------|---------------------------------------------|
| Skia       | <10ms   | 20×+    | Fix isotropic bug → native SkImageFilter    |
| TinySkia   | ~400ms  | 35×     | Running-sum box blur + SIMD + cache opt     |

The 35× speedup needed for TinySkia breaks down roughly as:
- **Running-sum box blur:** ~10× (eliminates O(kernelSize) inner loop for σ≥2)
- **4-channel processing:** ~2× (one load serves all channels vs per-channel iteration)
- **SIMD (NEON):** ~2× (4-wide float multiply-accumulate on Apple Silicon)
- **Cache optimization (vertical pass):** ~1.5-2× (transpose eliminates column-wise cache misses)
- **Reduce float pipeline overhead:** ~1.5× (avoid unnecessary FloatPixmap round-trips)

### Metrics

- **Frame time** for Donner Splash SVG (3 blur filters, 1200×900 viewport at 2× density).
- **Isolated blur time** for 1024×1024 pixmap at σ=6 (representative of common artwork).
- **resvg test suite throughput** — wall-clock time for full suite execution.

### Baselines to Establish

Before any optimization work, capture:
1. Skia backend frame time for Donner Splash (currently using CPU fallback — 200ms).
2. Skia backend frame time after isotropic fix (native path).
3. TinySkia backend frame time for Donner Splash (currently 14s).
4. Isolated `gaussianBlur()` call time for 1024×1024 at σ=1, 3, 6, 12.
5. `FloatPixmap::fromPixmap()` / `toPixmap()` round-trip time for 1024×1024.

### Regression Testing

All optimizations must pass the existing resvg test suite at current thresholds. The running-sum
box blur should produce bit-identical output to the current direct convolution (same rounding).
SIMD paths must be validated against scalar paths with exact-match comparisons.

---

## File Inventory

| File | Role | Changes |
|------|------|---------|
| `RendererSkia.cc` | Skia filter dispatch | Fix isotropic bug, extend eligibility, add native lowering |
| `GaussianBlur.cpp` | CPU blur implementation | Running sum, SIMD, cache optimization |
| `FloatPixmap.h` | Float pixel buffer | SIMD conversion, alignment |
| `FilterGraph.cpp` | Filter graph executor | No changes (routing stays the same) |
| `FilterGraphExecutor.cc` | SVG→pixel-space bridge | No changes |
| `SimdHelpers.h` (new) | SIMD abstraction layer | NEON/SSE2/scalar |
| `Morphology.cpp` | Erode/dilate | van Herk/Gil-Werman (Phase 4) |
| `BUILD.bazel` | Build config | SIMD compile flags, feature detection |

## Skia Source Reference

Skia source (BSD-licensed, same as Donner) is available at:
`external/+_repo_rules3+skia/` (relative to `bazel info output_base`)

Key reference files for native filter lowering:
- `include/effects/SkImageFilters.h` — All 17 SkImageFilter factory methods
- `include/core/SkColorFilter.h` — `Matrix()`, `TableARGB()`, `SRGBToLinearGamma()`,
  `LinearToSRGBGamma()`
- `include/effects/SkPerlinNoiseShader.h` — `MakeTurbulence()`, `MakeFractalNoise()`
- `modules/svg/src/SkSVGFe*.cpp` — Skia's own SVG filter lowering (reference implementation)
- `modules/svg/src/SkSVGFilterContext.cpp` — Named result registry, input resolution, colorspace
- `src/effects/imagefilters/SkBlurImageFilter.cpp` — Blur kernel implementation
