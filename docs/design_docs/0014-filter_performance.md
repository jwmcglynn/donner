# Filter Performance Optimization

**Status:** Complete (all filters within 1.5x of Skia)
**Author:** jwm
**Date:** 2025-03-11
**Related:** [renderer_interface_design.md](./0003-renderer_interface_design.md)

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
- **TinySkia backend:** Achieve performance within **1.5× of the Skia backend** for equivalent
  workloads. Currently 70× slower (14s vs 200ms for Donner Splash). Target: ~300ms or less.
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
slower. Target: within **1.5× of the Skia backend** for equivalent workloads (Donner Splash: ~300ms
vs Skia's ~200ms). This requires a ~47× improvement from the current 14s baseline.

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

### Phase 1: Quick Wins — Skia Native + Blur Algorithm ✅

1. ✅ **Fix the inverted isotropic blur check** in `getSimpleNativeSkiaBlur()`.
2. ✅ **Relax filterRegion rejection** — allow `filterRegion` with `CropRect` wrapping.
3. ✅ **Implement running-sum box blur** — O(1) sliding window for σ ≥ 2.0.
4. ✅ **Process 4 channels together** — scalar 4-channel accumulation.

### Phase 2: Skia Full Native Lowering ✅

5. ✅ **Build `SkImageFilter` DAG from `FilterGraph`** — `buildNativeSkiaFilterDAG()` replaces
   `getSimpleNativeSkiaBlur()`. Walks the full graph, wires inputs as constructor arguments.
6. ✅ **Lower all simple primitives**: feOffset, feFlood, feBlend, feMerge, feDropShadow,
   feColorMatrix, feMorphology.
7. ✅ **Lower spatial/generative primitives**: feTurbulence, feDisplacementMap, feConvolveMatrix.
   - feTile and feImage still fall back to CPU (need runtime context).
8. ✅ **Lower lighting primitives**: feDiffuseLighting, feSpecularLighting (all 3 light types).
9. ✅ **Lower feComponentTransfer**: Pre-compute 256-entry LUT per channel, use TableARGB().
10. ✅ **Handle feComposite arithmetic mode** via `SkImageFilters::Arithmetic()`.

### Phase 3: SIMD + Cache Optimization ✅

11. ✅ **NEON intrinsics** — inline in FloatPixmap.h and GaussianBlur.cpp (no separate header
    needed since NEON is always available on ARM64).
12. ✅ **SIMD-accelerate FloatPixmap conversion** — NEON uint8↔float (16 bytes at a time).
13. ✅ **SIMD-accelerate horizontal blur** — NEON vmlaq_f32 for weighted, vaddq/vsubq for box.
14. ✅ **Vertical pass cache optimization** — transpose-blur-transpose with 32×32 block tiling.
15. ✅ **Audited FloatPixmap round-trips** — filter graph executor already operates in float;
    no unnecessary uint8 round-trips.
16. ✅ **Fast `approxPowf()` for sRGB conversion** — replaced `std::pow()` (~50 cycles) with
    bit-trick approximation (~3-5 FLOPs) in float ColorSpace conversion.

### Phase 4: Polish ✅

17. ✅ **van Herk/Gil-Werman morphology** — O(w×h) separable erode/dilate with
    transpose-based vertical pass.

### Phase 5: Complete Native Skia Coverage ✅

18. ✅ **feTile native Skia lowering** — `SkImageFilters::Tile(srcRect, dstRect, input)` with
    subregion resolution for source/dest rects. Tracks per-node subregions for input resolution.
19. ✅ **feImage native Skia lowering** — creates `SkImage` from pre-loaded pixel data,
    applies preserveAspectRatio mapping. Supports both regular images and fragment references.

All 17 SVG filter primitives now lower to native `SkImageFilter` chains on the Skia backend.

### Phase 6: Benchmark Suite ✅

20. ✅ **Filter benchmark suite** — Google Benchmark-based microbenchmarks for blur, color space
    conversion, morphology, and FloatPixmap conversion. Located at
    `third_party/tiny-skia-cpp/tests/benchmarks/FilterPerfBench.cpp`.
21. ✅ **Filter perf regression test** — automated guard checking algorithmic invariants:
    - Blur sigma invariance (σ=20/σ=6 ≈ 1.0) — detects O(kernelSize) regression
    - Morphology radius invariance (r=30/r=3 < 2.0) — detects O(r²) regression
    - Float/uint8 blur ratio (< 2.5×) — detects float path regression
22. ✅ **Updated render perf regression baseline** — stroke_path/simd_over_scalar baseline
    raised from 1.20 to 1.50 to match improved SIMD stroke performance.

### Phase 7: Per-Filter SIMD Optimization (Partial)

Applied Vec4f32 SIMD and algorithmic optimizations to all filter float paths:

23. ✅ **Blend** — Premultiplied fast paths for Normal, Multiply, Screen (no unpremultiply needed).
    Switch hoisting moves blend mode dispatch outside pixel loop. 5-7x speedup.
24. ✅ **Composite** — Vec4f32 SIMD for all Porter-Duff operators. Pre-splatted k1-k4 constants
    for arithmetic mode. 2x speedup.
25. ✅ **ColorMatrix** — Pre-converted double→float matrix, direct pointer access. 1.1x speedup.
    Already within 1.5x of Skia (1.8x → optimized further).
26. ✅ **ConvolveMatrix** — Interior/border split (99% of pixels skip bounds checks), Vec4f32
    accumulation with pre-converted float kernel. 2.6x speedup.
27. ✅ **Lighting** — Precomputed alpha buffer eliminates 9 per-pixel span accesses in Sobel normal.
    All-float helpers (`normalizeF`, `pointLightDirectionF`). `fastPowF` binary exponentiation for
    integer specular exponents (5 multiplies vs ~50 cycle `std::pow`). 1.3-2.1x speedup.
28. ✅ **DisplacementMap** — Precomputed displacement values avoid per-pixel division for
    unpremultiply. Inline bilinear sampling with in-bounds fast path. 1.4x speedup.

### Phase 8: uint8 Pipeline (Align with Skia Architecture) — Deprioritized

**Original rationale:** Believed Skia was 33-110x faster; switching to uint8 storage would reduce
memory bandwidth 4x and close the gap.

**Updated status (2026-03-12):** After fixing the Skia benchmark cache bug, all filters are already
within the 1.5x target. The uint8 blur path (2.09ms) is already 2.3x faster than Skia (4.78ms).
Switching to uint8 storage is no longer performance-critical. The float pipeline provides better
precision and all filters already meet the 1.5x target. Keeping this as potential future work if
specific workloads reveal bandwidth bottlenecks.

29. ☐ **Switch FilterGraph to uint8 storage** — optional, for bandwidth-sensitive scenarios
30. ☐ **Update FilterGraph.h** — change paint inputs from FloatPixmap to Pixmap
31. ☐ **Update FilterGraphExecutor.cc** — remove uint8→float conversion on graph setup

### Phase 9: Render Performance Benchmarks ✅

32. ✅ **Skia render benchmarks** (`donner/benchmarks/SkiaRenderPerfBench.cpp`) — FillPath,
    FillRect, StrokePath, FillPath_LinearGradient, FillPath_Opaque at 512².
33. ✅ **tiny-skia render benchmarks** (`donner/benchmarks/TinySkiaRenderPerfBench.cpp`) —
    matching operations for direct comparison.

### Phase 10: Render Performance Optimization ✅

34. ✅ **Fused linear gradient 2-stop stage** — `FusedLinearGradient2Stop` pipeline stage combines
    SeedShader+Transform+PadX1+EvenlySpaced2StopGradient+Premultiply into a single function call
    with NEON intrinsics. Eliminates 4 stage dispatches and 3 join/split memory round-trips per
    16-pixel batch, plus uses register-resident float32x4_t to avoid F32x8T load/store overhead.
    **Result: 6.53x → 1.11x** (matches Skia).
35. ✅ **blitAntiRect batching** — `RasterPipelineBlitter::blitAntiRect()` override batches edge
    columns and interior into 3 calls (from 3×height). Minor improvement for large shapes.
36. ✅ **Stroke tolerance scaling** — Loosened `invResScale_` for thin strokes (radius < 2px) to
    reduce round join/cap subdivision. Conservative 2x max scale to stay within test thresholds.
37. ✅ **AdditiveBlitter flush optimization** — Replaced RLE-based flush (2 vector zero-fills +
    encoding/decoding) with direct blit emission (blitH for full-coverage runs, blitAntiH2 for
    partial pixels). Added dirty range tracking to avoid scanning the full row width.
38. ✅ **Scan converter heap allocation elimination** — Increased `kQuickLen` from 31 to 513 in
    `blitAaaTrapezoidRow`, eliminating per-row `new[]`/`delete[]` for scanlines up to 513px.
39. ✅ **Inline SourceOver blend** — Added fast paths to `blitAntiH2` and `blitV` that bypass the
    full pipeline dispatch for solid color SourceOver. For opaque Source (strength-reduced from
    SourceOver), uses single-div255 lerp. For semi-transparent SourceOver, uses inline ScaleU8 +
    SourceOver. Eliminates ~40ns pipeline overhead per 2-pixel blit call.
    **Key insight:** The 16-wide SIMD pipeline has high fixed overhead for 2-pixel edge blits
    (12.5% utilization). Inline scalar blend is ~10x faster per pixel for these narrow blits.
40. ✅ **Conic tolerance scaling** — Added configurable conic-to-quad tolerance to PathBuilder.
    Stroker uses looser tolerance for thin strokes (radius < 4px, up to 4x tolerance increase),
    reducing quad count from round caps/joins.

### Phase 11: Expanded filter benchmark coverage ✅

Added benchmarks for all remaining SVG filter primitives (Flood, Offset, Merge, ComponentTransfer,
Tile) and optimized the ones that exceeded the 1.5x target:

41. ✅ **Merge NEON vectorization** — Replaced scalar `double` SourceOver with integer-only div255
    formula + NEON 4-pixel-at-a-time vectorization. 9.13x → 1.19x (7.7x speedup).
42. ✅ **Tile row-based memcpy** — Replaced per-pixel modulo arithmetic with row-based memcpy using
    precomputed tile boundaries. 3.79x → 0.23x (16x speedup, now faster than Skia).
43. ✅ **Uint8 benchmark paths** — Changed filter benchmarks to test uint8 code paths for operations
    that don't require linearRGB (Flood, Offset, Merge, ComponentTransfer, Tile), matching the
    actual execution path used by the filter graph.

---

## Benchmarking

### Filter Performance: tiny-skia vs Skia (512²)

After Phase 7 per-filter SIMD optimizations + Phase 11 expanded coverage:

| Filter | tiny-skia | Skia | Ratio | Status |
|--------|----------|------|-------|--------|
| Blur float σ=6 | 6.45ms | 4.83ms | **1.34x** | ✅ Within 1.5x |
| Blur uint8 σ=6 | 2.08ms | 4.83ms | **0.43x** | ✅ tiny-skia 2.3x FASTER |
| Blur float 1024² σ=6 | 27.1ms | 19.1ms | **1.42x** | ✅ Within 1.5x |
| Blur uint8 1024² σ=6 | 9.6ms | 19.1ms | **0.50x** | ✅ tiny-skia 2x FASTER |
| Morphology dilate r=3 | 3.06ms | 22.0ms | **0.14x** | ✅ tiny-skia 7x FASTER |
| Morphology erode r=10 | 3.50ms | 57.5ms | **0.06x** | ✅ tiny-skia 16x FASTER |
| Blend Multiply | 0.32ms | 0.49ms | **0.66x** | ✅ tiny-skia 1.5x FASTER |
| Blend Screen | 0.24ms | 0.49ms | **0.49x** | ✅ tiny-skia 2x FASTER |
| Composite Over | 0.23ms | 0.46ms | **0.51x** | ✅ tiny-skia 2x FASTER |
| Composite Arithmetic | 0.29ms | 3.58ms | **0.08x** | ✅ tiny-skia 12x FASTER |
| ColorMatrix Saturate | 0.42ms | 0.50ms | **0.84x** | ✅ tiny-skia 1.2x FASTER |
| Convolve 3×3 | 2.55ms | 47.7ms | **0.05x** | ✅ tiny-skia 19x FASTER |
| Convolve 5×5 | 6.13ms | 121.0ms | **0.05x** | ✅ tiny-skia 20x FASTER |
| Turbulence | 4.62ms | 6.78ms | **0.68x** | ✅ tiny-skia 1.5x FASTER |
| FractalNoise | 4.83ms | 6.80ms | **0.71x** | ✅ tiny-skia 1.4x FASTER |
| DiffuseLighting | 1.80ms | 12.1ms | **0.15x** | ✅ tiny-skia 7x FASTER |
| SpecularLighting | 5.09ms | 15.4ms | **0.33x** | ✅ tiny-skia 3x FASTER |
| DisplacementMap | 1.75ms | 2.74ms | **0.64x** | ✅ tiny-skia 1.6x FASTER |
| **Flood** | 0.02ms | 0.11ms | **0.18x** | ✅ tiny-skia 6x FASTER |
| **Offset** | 0.02ms | 0.06ms | **0.39x** | ✅ tiny-skia 3x FASTER |
| **Merge 3-Input** | 0.32ms | 0.27ms | **1.19x** | ✅ Within 1.5x |
| **ComponentTransfer Table** | 0.76ms | 0.75ms | **1.01x** | ✅ Within 1.5x |
| **Tile 64×64** | 0.02ms | 0.07ms | **0.23x** | ✅ tiny-skia 3x FASTER |

**All 23 filter benchmarks are within the 1.5x target.** We're faster than Skia on 21 out of 23.

#### Critical benchmark bug fix (2026-03-12)

Previous benchmarks showed 33-110x gaps because Skia's `SkImageFilterCache` was caching results
across benchmark iterations. The cache key includes (filter unique ID, transform matrix, clip
bounds, source image ID) — all constant across iterations. Only the first iteration computed the
filter; subsequent iterations returned the cached result (~60μs blit time). Adding
`SkGraphics::PurgeResourceCache()` at the start of each iteration reveals the true performance.

### Render Performance: tiny-skia vs Skia (512²)

| Operation | tiny-skia | Skia | Ratio | Status |
|-----------|----------|------|-------|--------|
| FillPath (semi-transparent) | 100μs | 130μs | **0.77x** | ✅ tiny-skia faster |
| FillRect (semi-transparent) | 74μs | 130μs | **0.57x** | ✅ tiny-skia faster |
| StrokePath (3px round) | 62μs | 45μs | **1.39x** | ✅ Within 1.5x (was 3.28x) |
| FillPath LinearGradient | 201μs | 201μs | **1.00x** | ✅ Matching Skia (was 6.53x) |
| FillPath Opaque | 45μs | 33μs | **1.37x** | ✅ Within 1.5x (was 3.02x) |
| FillPath RadialGradient | 233μs | 216μs | **1.08x** | ✅ Within 1.5x (was 7.23x) |
| StrokePath Dashed | 120μs | 92μs | **1.26x** | ✅ Within 1.5x |
| StrokePath Thick (10px) | 69μs | 51μs | **1.33x** | ✅ Within 1.5x |
| FillPath Transformed (30°) | 105μs | 135μs | **0.78x** | ✅ tiny-skia faster |
| FillPath EvenOdd | 104μs | 133μs | **0.78x** | ✅ tiny-skia faster |
| FillPath Pattern (64×64 tile) | 86μs | 72μs | **1.22x** | ✅ Within 1.5x (was 9.25x) |

**All 11 render operations are within 1.5x of Skia. 4 operations are faster than Skia.**

Both `render_perf_compare` and `filter_perf_compare` tests enforce a **1.5x threshold** — the test
fails if any operation exceeds 1.5x of Skia, serving as a regression gate.

**Completed optimizations:**

- **LinearGradient: 6.53x → 1.00x** — Fused `FusedLinearGradient2Stop` pipeline stage replaces
  5 separate stages (SeedShader+Transform+PadX1+EvenlySpaced2StopGradient+Premultiply) with a
  single NEON-intrinsic implementation. Key insight: F32x8T's abstraction caused load→operate→store
  on every operation (no register reuse). The fused NEON path keeps all values in float32x4_t
  registers throughout, using `vfmaq_f32` fused multiply-add for gradient interpolation. Result:
  ~6x speedup, now matching Skia.

- **RadialGradient: 7.23x → 1.08x** — Fused `FusedRadialGradient2Stop` pipeline stage for simple
  radial gradients (same center, startRadius=0, 2-stop, Pad mode). Combines SeedShader+Transform+
  XYToRadius+PadX1+EvenlySpaced2StopGradient+Premultiply into a single function. NEON path uses
  `vsqrtq_f32` for vectorized sqrt, processes 4 groups of 4 pixels. Now matches Skia.

- **Pattern: 9.25x → 1.22x** — Fused `FusedBilinearPattern` pipeline stage with eight
  optimization tiers:
  1. **u8→u16 direct conversion**: Eliminated float intermediate (was: u8→float/255→process→
     float*255→u16; now: u8→u16 directly). Saves 64+ FLOPs per 16-pixel batch.
  2. **NEON vectorized transform+tile**: Groups of 4 pixels transformed with `vfmaq_f32`,
     repeat tiling with vectorized `vrndmq_f32` (floor) + fract operation.
  3. **Sequential tile row load (vld4q_u8)**: For identity-like transforms (sx=1, kx=0, ky=0),
     consecutive output pixels map to consecutive tile pixels. Uses NEON `vld4q_u8` to
     deinterleave 16 RGBA pixels in a single instruction + `vmovl_u8` widening. With wrapping:
     2 memcpy + vld4q; without wrapping: single vld4q directly from tile row.
  4. **Fused SourceOver+Store (peephole)**: Compile-time peephole detects [FusedBilinearPattern,
     SourceOverRgba] and sets `fuseSourceOver=true`, eliminating 3 function pointer calls and
     384 bytes of U16x16T register marshalling per batch. NEON path uses direct `vmull_u8` +
     `vrshrn_n_u16` for u8-native SourceOver blend within the pattern stage.
  5. **Scanline-level processing + scanlineDone**: For identity+repeat+fuseSourceOver, processes
     entire scanline in one call. Added `scanlineDone` flag to Pipeline struct so the `start()`
     loop breaks immediately after the first batch, eliminating ~30 no-op function pointer calls
     per scanline (each checking early-return condition). (1.72x → improvement was ~20μs saved.)
  6. **Tile-aligned segments + opaque memcpy**: Splits scanline at tile boundaries to avoid
     wrapping memcpy and integer modulo. Detects fully-opaque tiles (via `vminvq_u8` on all alpha
     values) and uses direct `memcpy` instead of blending.
  7. **Fused AA edge pipeline (peephole)**: Second peephole detects [FusedBilinearPattern,
     Scale1Float, LoadDestination, SourceOver, Store] (the blitAntiH AA edge pipeline) and fuses
     into a single stage with coverage scaling.
  8. **Inline pattern blend in blitAntiH2**: The dominant bottleneck was `blitAntiH2`, called
     ~3600 times per frame for individual edge pixels. Each call dispatched a 16-wide SIMD
     pipeline (12.5% utilization) through 5 stages. Added inline scalar pattern blend: direct
     tile pixel lookup + coverage scaling + SourceOver blend, completely bypassing the pipeline.
     Each call went from ~70ns (pipeline overhead) to ~5ns (inline). Savings: ~3600 × 65ns =
     ~234μs → brought pattern from 1.72x to 1.22x.

- **blitAntiRect batching** — Overrode `RasterPipelineBlitter::blitAntiRect()` to batch edge
  columns (single blitV for full height) and interior (single blitRect), replacing per-row
  decomposition. Reduces function call overhead from 3×height to 3 calls.

- **Stroke tolerance scaling** — For thin strokes (radius < 2px), loosened subdivision tolerance
  proportionally (up to 2x). Reduces generated quad segments for round joins/caps with no visible
  quality loss. GhostscriptTiger pixel diff went from 0 to 149 (within 200 threshold).

- **Scan converter flush optimization: 3.02x → 2.27x** — Rewrote `AdditiveBlitter::flush()` to
  emit direct blit calls (blitH for 0xFF runs, blitAntiH2 for partial pixels) instead of encoding/
  decoding RLE alpha runs. Added dirty range tracking (dirtyMin_/dirtyMax_) to avoid scanning full
  row width. Increased kQuickLen from 31→513 to eliminate heap allocations for typical scanlines.

- **Inline SourceOver blend: 2.27x → 1.36x (opaque), 2.93x → 1.37x (stroke)** — Added scalar
  inline blend fast paths in `blitAntiH2()` and `blitV()` for narrow (1-2 pixel) edge blits.
  The 16-wide SIMD pipeline has ~40ns fixed overhead at 12.5% utilization for 2-pixel dispatches.
  Inline scalar blend is ~10x faster per pixel for these narrow cases. Dual-path: opaque Source
  uses simple lerp (`memsetColor_`), semi-transparent SourceOver uses ScaleU8+SourceOver formula
  (`solidSrcOverColor_`).

### How Skia Handles Color Space in Filters

Skia stores filter intermediate results as uint8 (`kN32_SkColorType` = RGBA 8888) but does ALL
math in float32 via its highp raster pipeline. For `color-interpolation-filters="linearRGB"`:

1. Each filter node is wrapped with `SkColorFilters::SRGBToLinearGamma()` on input
2. The sRGB→linear conversion uses the `parametric` raster pipeline op (highp-only, float32)
3. Filter math runs in float32
4. Results quantize back to uint8 between nodes

This means Skia accepts ±1/255 quantization between filter stages. The precision loss is visually
imperceptible for typical filter chains. Our current float-throughout approach is more precise but
carries a 4x bandwidth penalty.

### Regression Testing

All optimizations must pass the existing resvg test suite at current thresholds. When switching
to uint8 pipeline (Phase 8), some tests may need threshold adjustments due to different
quantization behavior (matching resvg's uint8 pipeline may actually improve some results).

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
| `Pipeline.h` | Pipeline stage enum/context | FusedLinearGradient2Stop, FusedRadialGradient2Stop, FusedBilinearPattern stages + contexts |
| `Pipeline.cpp` | Stage dispatch | Updated stage count (84), lowp compatibility for fused stages |
| `Lowp.cpp` | Low-precision pipeline | NEON fused gradient + pattern stages with vld4q fast path |
| `Highp.cpp` | High-precision pipeline | Fused gradient + pattern stages (scalar) |
| `Gradient.cpp` | Gradient shader | tryPushFusedLinear2Stop(), tryPushFusedRadial2Stop() fast paths |
| `LinearGradient.cpp` | Linear gradient | Routes through fused stage when eligible |
| `RadialGradient.cpp` | Radial gradient | Routes simple radials through fused stage |
| `Pattern.cpp` | Pattern shader | Fused pattern stage for Nearest/Bilinear with Repeat tiling |
| `Blitter.h` / `PipelineBlitter.cpp` | Blitting | blitAntiRect override for batched operations |
| `Stroker.cpp` | Stroke expansion | Thin-stroke tolerance scaling |

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
