# Design: Geode — GPU-Native Rendering Backend

**Status:** Phases 0–3 + Phase 5b landed on main (Phase 0 #481; Phase 1 #484 + #492; Phase 2 #497; Phase 5b MSAA + resvg parity — 596 passing, 0 failing — #504; Phase 3 clip/mask #506); vendor story swapped from Dawn-from-source to prebuilt wgpu-native in #510; first real-GPU verification on 2026-04-17 (Intel Arc A380 / Mesa Xe-KMD Vulkan — smoke + 1-band path-fill green; multi-band paths hung on Xe-KMD sample_mask output; vendor-gated alpha-coverage shader fallback shipped in #536). Phase 3d (blend modes + miter + markers) and Phase 4 (text) remain unshipped; `RendererGeode::drawText` and `pushFilterLayer` are still stubs.
**Author:** Jeff McGlynn
**Created:** 2026-04-07
**Last updated:** 2026-04-17

## Implementation status

- **Phase 0** (Type refactoring): ✅ complete, merged in #481.
  Replaces `PathSpline` with immutable `Path` + `PathBuilder`, moves `FillRule`
  into `donner/base`, adds `BezierUtils`, migrates all renderers/systems.
- **Phase 1** (Foundation + path rendering): 🚧 in progress. #484 is the
  MVP covering Dawn vendoring and `GeodeDevice`. Landed pieces:
  - ✅ Dawn WebGPU vendored via `rules_foreign_cc` + CMake (see Phase 1 section).
  - ✅ `GeodeDevice`: headless WebGPU device/queue factory.
  - ✅ End-to-end GPU draw verified (clear-to-red + texture readback).
  - ✅ Slug WGSL fill shader compiles via Dawn's Tint compiler.
  - ✅ `GeodePathEncoder` Slug band decomposition (commit `e42f3f75`).
  - ✅ `GeoEncoder` + `GeodePipeline` — first GPU-rendered SVG paths
    (clear, fillRect, fillTriangle, fillCircle all green; commit `ddbcda6b`).
  - ✅ `RendererGeode` skeleton — solid-fill `drawPath`/`drawRect`/
    `drawEllipse` through the `RendererInterface` adapter, stubs for
    clip/mask/layer/filter/pattern/image/text.
  - 🚧 Stroke rendering via `Path::strokeToFill()` → Slug fill pipeline.
    Basic plumbing landed (axis-aligned rects, open-subpath polylines,
    variable widths); multiple `Path::strokeToFill` limitations remain
    (dashes, round/square caps, sharp concave corners on open subpaths,
    curved flattened strokes on closed subpaths). See Phase 2 checklist
    for details and follow-up tasks.
  - ✅ `--config=geode` backend selection — sets both
    `renderer_backend=geode` and `enable_geode=true`. Default builds are
    unaffected (Dawn still gated off).
  - ✅ Golden image tests for solid-fill SVGs — 5/5 green.
    `renderer_geode_golden_tests` uses per-backend goldens under
    `testdata/golden/geode/` (the Skia/tiny-skia goldens in `golden/`
    don't match Slug's winding-number AA at edge pixels, so Geode has
    its own). Curated suite: `MinimalClosedCubic2x2`,
    `MinimalClosedCubic5x3`, `BigLightningGlowNoFilterCrop`, `Lion`,
    `Edzample`. Strict identity check (`threshold=0`, `max=0`,
    `includeAntiAliasingDifferences`) catches any Geode-side regressions.
    Regenerate with `UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace)
    bazel run --config=geode //donner/svg/renderer/tests:renderer_geode_golden_tests`.
  - 🚧 Linux CI via Mesa `llvmpipe` — switched from SwiftShader plan.
    Ubuntu's `mesa-vulkan-drivers` package provides `llvmpipe`, a
    maintained software Vulkan ICD that's apt-installable (no vendoring
    required). Dawn auto-discovers it via the standard Vulkan loader.
    Added as an experimental `linux-geode` CI job
    (`continue-on-error: true` until the first run confirms it works).
- **Phase 2**: 🚧 in progress.
  - ✅ `drawImage`: textured quads via a dedicated image-blit pipeline
    (`GeodeImagePipeline`) + reusable texture upload/draw helpers
    (`GeodeTextureEncoder`). Supports bilinear and nearest sampling
    (`image-rendering: pixelated`), `ImageParams::opacity` combined with
    `paint.opacity`, and honors the current transform stack. Texture
    uploads go through `wgpu::Queue::WriteTexture` with `bytesPerRow`
    normalized to 256-byte alignment on the slow path (the fast path
    uploads directly when `width*4` is already aligned). `GeoEncoder`
    now `SetPipeline`s the Slug fill pipeline on every `fillPath` so
    it's safe to interleave fills and image draws within one pass.
    **Reusable for Phase 2H patterns:** `GeodeTextureEncoder::drawTexturedQuad`
    takes a pre-uploaded `wgpu::Texture` plus explicit `destRect`/`srcRect`
    in target-pixel / UV space. Phase 2H will render the pattern tile to
    an offscreen texture (via `GeoSurface`), then call `drawTexturedQuad`
    with the repeating `srcRect` to stamp the tile across the fill region.
- **Phase 3** (Compositing and clipping): ✅ complete, merged in #506.
  Polygon clipping, path-based clipping via resolved R8 coverage masks, and
  `<mask>` element compositing via luminance blit. Nested `<g>` clips and
  `maskUnits=userSpaceOnUse` / percent-sized bounds handled. Unlocked the
  full `masking/clipPath`, `masking/clip`, `masking/clip-rule`, and
  `masking/mask` categories on the resvg test suite.
- **wgpu-native vendor swap**: ✅ merged in #510. Replaced the Dawn
  `rules_foreign_cc` + CMake-from-source build with prebuilt
  `wgpu-native` v24.0.3.1 archives consumed via `http_archive`. Cuts a
  cold CI build from ~1 h 45 m to seconds. See the "Bazel vendoring
  strategy (wgpu-native)" section under Background for the current
  authoritative vendoring design; the "Historical: Dawn embedding
  strategy" section is retained for context only. The user-visible
  `--config=geode` / `enable_geode=true` flags are unchanged.
- **Phase 3d** (mix-blend-mode): ✅ complete, merged in #541. Implements
  all 16 SVG/CSS `mix-blend-mode` operators (Multiply, Screen, Overlay,
  Darken, Lighten, ColorDodge, ColorBurn, HardLight, SoftLight,
  Difference, Exclusion, Hue, Saturation, Color, Luminosity) via an
  extended `image_blit.wgsl` with a `blendMode` uniform and a
  `dstSnapshotTexture` binding. Non-Normal blend modes take a snapshot
  of the current target and composite via the shader's in-line blend
  functions. Lifts the `painting/mix-blend-mode` category gate in the
  resvg suite.
- **Phase 4** (Text rendering): ✅ `drawText` implemented.
  Routes shaped-glyph outlines through the Slug fill pipeline via the
  existing `drawPath` path. Enables text rendering for direct Geode
  consumers. The resvg `text/*` category gate stays closed -- Geode's
  4x MSAA produces ~600-800 px AA drift per glyph vs tiny-skia's 16x
  supersample reference; unlocking those 268 tests requires a finer
  sample pattern or analytic glyph AA (Phase 5 follow-up).
- **Real-GPU verification (2026-04-17)**: 🚧 first run on real hardware,
  plus a targeted fallback shader path for Intel+Vulkan. Added
  adapter-info logging to `GeodeDevice::CreateHeadless` (commit
  `5f6ac7d4`). On Intel Arc A380 (DG2) with Mesa 25.2.8 Xe-KMD Vulkan
  the smoke test (`GeodeDevice.CanExecuteClearAndReadback`) and
  `DrawPathWithSolidFill` pass, but any path with `bandCount >= 2`
  hung indefinitely due to a Mesa ANV driver bug in
  `@builtin(sample_mask)` output when two fragment invocations at the
  same pixel both write it (exactly the Slug half-pixel band overlap).
  Experimentally confirmed: the same tests pass under Mesa llvmpipe,
  proving Geode's pipeline is correct. #536 ships three alpha-coverage
  WGSL shader variants (`slug_fill_alpha_coverage.wgsl`,
  `slug_gradient_alpha_coverage.wgsl`, `slug_mask_alpha_coverage.wgsl`)
  vendor-gated on `vendorID == 0x8086 && backendType == Vulkan`. Mesa
  25.3 upstream fix exists; once CI Mesa crosses that version, the
  fallback can be deleted. Known follow-up (issue #537): band-boundary
  pixels in the alpha-coverage fallback lose coverage — cosmetic AA
  artifact on the Intel-Vulkan path only, does not affect default
  MSAA + sample_mask rendering.
  **1-sample alpha-coverage variant**: When `useAlphaCoverageAA` is
  active, all pipelines run at `sampleCount = 1` (no MSAA texture, no
  hardware resolve). The alpha-coverage shaders compute 4-sample
  supersampling in the fragment shader and fold coverage into alpha, so
  hardware MSAA is unnecessary overhead. `GeodeDevice::sampleCount()`
  returns 1 on the alpha-coverage path, 4 otherwise; all pipeline
  constructors, `GeoEncoder`, and `RendererGeode` MSAA-texture
  allocations gate on this value. Note: a separate class of
  non-deterministic GPU hangs (~20% per-submission) remains on
  Arc A380 + Mesa ANV 25.2.8 — these affect even empty render passes
  (clear + readback, no shader execution) and are a driver/hardware
  bug independent of MSAA or shader variant.


## Summary

Geode is a new GPU-native rendering backend for Donner, built on WebGPU and the Slug algorithm
for resolution-independent vector rendering. It replaces the CPU rasterization approach of
tiny-skia-cpp and the heavyweight Skia dependency with a purpose-built GPU pipeline that renders
SVG paths, text, and compositing operations directly on the GPU without tessellation or texture
atlases.

The backend implements Donner's existing `RendererInterface`, sharing the `RendererDriver`
traversal logic with all other backends, while introducing deep ECS integration for GPU resource
caching and incremental updates. The design prioritizes embeddability — Geode is intended to be
usable inside game engines, UI frameworks, and other applications that already own a GPU context.

## Goals

- Implement `RendererInterface` with full SVG rendering support (v1: everything except filters;
  v2: filters).
- Use WebGPU as the primary graphics API, with a future path to native Vulkan/Metal via
  MoltenVK.
- Implement the Slug algorithm for GPU-based resolution-independent path and text rendering
  without tessellation or glyph atlases.
- Provide a donner-native API surface comparable to tiny-skia-cpp's drawing primitives, using
  donner types (`Transform2d`, `Path`, `Box2d`, `Vector2d`, etc.) instead of third-party
  types.
- Integrate with the ECS registry for GPU resource caching, enabling efficient incremental
  re-rendering when the scene graph changes.
- Design for embeddability: allow host applications to provide their own GPU device/queue,
  render into caller-owned textures, and interleave Geode rendering with other GPU work.
- Serve as a general-purpose 2D graphics library — the `GeoEncoder`/`GeoSurface` API should
  be usable standalone for any 2D drawing workload (UI rendering, data visualization, creative
  tools, game 2D layers), not only SVG rendering through `RendererInterface`.
- Maintain the build-time backend selection model — Geode is selected via `--config=geode` in
  Bazel or `-DDONNER_RENDERER_BACKEND=geode` in CMake.

## Non-Goals (for initial phases)

- Runtime backend switching (remains build-time selected).
- Replacing tiny-skia-cpp or Skia backends — they continue to serve their roles (lightweight
  CPU, reference/full-featured).
- General-purpose 3D scene graph or physics — Geode is a rendering engine, not a game engine
  framework. 3D support (future) is limited to rendering 2D/3D vector and mesh content.
- WebGPU compute shaders for filter effects in v1 (deferred to v2).
- Window management, input handling, or application framework — Geode renders into surfaces
  that the host provides.

## Background

### Current Backends

Donner currently ships two rendering backends behind `RendererInterface`:

| Backend | Type | Binary Size | Strengths | Limitations |
|---------|------|-------------|-----------|-------------|
| FullSkiaRenderer | CPU/GPU (Skia) | ~50 MB | Full feature parity, reference quality | Heavy dependency, not embeddable |
| RendererTinySkia | CPU (software) | ~2 MB | Lightweight, no dependencies | No text, limited filters, CPU-bound |

Both backends are CPU-centric for rasterization. For applications requiring high-performance
rendering — real-time UI, game engines, large/complex SVGs — a GPU-native backend is the natural
next step.

### The Slug Algorithm

The [Slug algorithm](https://terathon.com/blog/decade-slug.html), developed by Eric Lengyel at
Terathon Software, renders vector graphics (including text glyphs) directly on the GPU from
Bézier curve data. The patent (US10373352B1) was permanently dedicated to the public domain on
March 17, 2026 via USPTO form SB/43. Reference implementations are available under the MIT
license on [GitHub](https://github.com/EricLengyel/Slug).

**Geode will implement the Slug algorithm from scratch under the ISC license.** We cannot use
the MIT reference shaders directly because Donner is ISC-licensed. The reference code and the
JCGT 2017 paper serve as algorithmic references for a clean-room implementation. The core
algorithm (winding number evaluation, root eligibility, band decomposition) is public domain
via the patent dedication; our implementation covers the shader code, data structures, and
CPU-side encoding pipeline.

**How it works:**

1. **No tessellation or atlases.** Curves are submitted directly to the GPU as band data — each
   path is decomposed into horizontal bands containing references to the curves that intersect
   that band.
2. **Winding number evaluation in the fragment shader.** For each pixel, the shader casts a ray
   and counts curve intersections to determine the winding number, then applies the fill rule
   (non-zero or even-odd). The root eligibility and winding number method is the mathematical
   core of Slug's robustness — it provably avoids dropped pixels, sparkles, or streak artifacts
   under all conditions including floating-point round-off.
3. **Dynamic dilation in the vertex shader.** Bounding polygons for each band are dilated by
   exactly half a pixel in viewport space, computed per-vertex using the MVP matrix and viewport
   dimensions. This replaced earlier approaches (fixed expansion constants, adaptive
   supersampling) and is strictly superior — it adapts automatically to glyph size, eliminates
   aliasing at small sizes without wasting GPU work on large glyphs, and handles perspective
   projection correctly.
4. **Resolution-independent.** Since curves are evaluated analytically per-pixel, rendering
   quality is identical at any zoom level — no atlas regeneration, no LOD switching.

**Key learnings from Slug's decade of production use:**

The ["A Decade of Slug" retrospective](https://terathon.com/blog/decade-slug.html) documents
critical lessons from deploying Slug at studios including Activision, Blizzard, id Software,
Ubisoft, Insomniac, and Adobe. Geode's design incorporates these learnings:

| Lesson | What happened | How Geode applies it |
|--------|--------------|---------------------|
| **Band-split optimization hurts more than it helps** | Duplicate sorted curve lists for bidirectional rays improved large glyphs but introduced shader divergence that hurt small text. Removed — halved band data from 4×16-bit to 2×16-bit per band. | Geode uses the simplified single-direction band format from day one. No bidirectional rays. |
| **Dynamic dilation obsoletes supersampling** | Adaptive supersampling was added for tiny text, then removed because dilation solved the same problem better with simpler shaders. | Geode implements dilation only; no supersampling path. Simpler fragment shader = faster compilation, less divergence. |
| **Per-glyph bounding polygons beat per-layer loops for color emoji** | Original multi-color emoji used a loop in the fragment shader over stacked layers. Most layers covered a small fraction of the composite glyph area, wasting work. Replaced with independent glyphs rendered as separate draw calls with individual bounding polygons. | Geode renders multi-layer color glyphs as independent instanced draws, not shader loops. Slightly more vertex data, significantly less fragment waste. |
| **Winding number core is stable** | The root eligibility and winding number calculation has been unchanged since 2017 — it was correct from the start. | Geode's clean-room implementation prioritizes matching the mathematical specification exactly. This is the one part where correctness is non-negotiable. |
| **2×16-bit band data is sufficient** | After removing band-split, each band needs only two 16-bit components (curve range start + count, or equivalent). | Geode packs band metadata into 32 bits per band. |

**Dynamic dilation detail:**

The vertex shader computes per-vertex expansion distance `d` along the outward normal `n̂` such
that the bounding polygon expands by exactly half a pixel in viewport space. Given the MVP
matrix `M` and viewport dimensions `(w, h)`:

- `s = M[3] · p` (homogeneous w-component of transformed vertex)
- `t = M[3] · n̂` (normal's contribution to w)
- `u = w · (s · (M[0] · n̂) - t · (M[0] · p))` (viewport-space x displacement)
- `v = h · (s · (M[1] · n̂) - t · (M[1] · p))` (viewport-space y displacement)

Solving the quadratic `(u² + v² - s²t²)d² - 2s³td - s⁴ = 0` yields:

```
d = (s³t + s²√(u² + v²)) / (u² + v² - s²t²)
```

The vertex is displaced by `d · n̂` in object space. A per-vertex 2×2 inverse Jacobian matrix
(stored as vertex attributes) maps this object-space displacement back to em-space coordinates
for correct curve sampling. This handles scale, stretch, skew, coordinate flips, and
perspective projection.

For Geode's 2D case (orthographic projection), the math simplifies significantly since `t = 0`
and `s` is constant, but the full perspective path is implemented from the start to support
`GeoEncoder3D` and `fillPath3D`.

**Advantages over alternatives:**

| Approach | Drawback Slug avoids |
|----------|---------------------|
| Tessellation (e.g., pathfinder, piet-gpu) | Vertex explosion on complex paths, LOD management |
| SDF textures (e.g., msdfgen) | Limited to simple glyphs, atlas management, blurriness at extremes |
| CPU rasterization (e.g., tiny-skia) | No GPU parallelism, memory bandwidth bound |
| Skia Ganesh/Graphite | Massive dependency, not designed for embedding |

Slug natively supports quadratic and cubic Bézier curves, lines, and arcs — covering the full
SVG path vocabulary. It has been proven at scale on hardware as modest as 2016-era game consoles
without significant frame rate impact.

### WebGPU

WebGPU provides a modern, portable graphics API that abstracts over Vulkan, Metal, and D3D12.
Geode ships against [wgpu-native](https://github.com/gfx-rs/wgpu-native) (the
Rust `wgpu` crate's C ABI surface) and uses
[eliemichel/WebGPU-distribution](https://github.com/eliemichel/WebGPU-distribution)'s
single-header `webgpu.hpp` C++ wrapper for idiomatic RAII handles. This gives us:

- Cross-platform support (Windows, macOS, Linux, Android, iOS, Web via wasm)
- Modern GPU features (compute shaders for v2 filters, storage buffers)
- No platform-specific code in the renderer
- Future path to native Vulkan/Metal for applications that need it

Geode originally embedded Google's Dawn (C++ WebGPU implementation) built from
source via `rules_foreign_cc`'s `cmake()` rule — see the "Historical: Dawn
embedding strategy" section below for the design notes and the reason for the
pivot. Everything user-visible (flag names, WGSL shaders, ECS integration)
carried over unchanged; only the native vendoring path moved from a
cmake-from-source build to a prebuilt-binary drop.

#### Bazel vendoring strategy (wgpu-native)

wgpu-native publishes pre-built release archives on its GitHub Releases page
for `{linux, macos, windows} × {x86_64, aarch64}`. We consume those directly
via `http_archive`: one repository per platform tuple, each carrying an
overlay `BUILD.wgpu_native_platform` file that exposes
`lib/libwgpu_native.{so,dylib}` plus the `include/webgpu/{webgpu,wgpu}.h`
headers as a single `cc_library`. `//third_party/webgpu-cpp` then `select()`s
the matching archive for the current `(os, cpu)` and aggregates the C
headers with the vendored `webgpu.hpp` C++ wrapper into one consumable
`//third_party/webgpu-cpp:webgpu_cpp` target.

Pins (see `//third_party/bazel/non_bcr_deps.bzl`):

- `wgpu-native` tag `v24.0.3.1` — the vendored `webgpu.hpp` tracks the v24
  C API shape (see `wgpu-native-git-tag.txt` in the upstream distribution).
  Bumping past v24 requires regenerating `webgpu.hpp` from the matching
  `wgpu-native` schema.
- SHAs for each zip are captured inline. Refresh them with `shasum -a 256`
  against the release asset when bumping.

This is opt-in: Geode's entire directory is gated behind
`--//donner/svg/renderer/geode:enable_geode=true` (default: false; flag name
is historical — kept stable to avoid churning command-line invocations).
Default `bazel test //...` never fetches wgpu-native, so contributors not
working on Geode pay zero time/disk cost for WebGPU.

**Fetch + build time:** the `http_archive` is a ~12 MB download; there is no
compile step on the critical path. On a cold GitHub Actions cache the
Dawn-from-source build previously cost ~1 h 45 m — the wgpu-native drop
completes in seconds. Incremental Geode edits recompile only the `donner/`
sources that include `webgpu.hpp`.

**On-disk layout:** each archive unzips to
`include/webgpu/{webgpu,wgpu}.h` plus `lib/libwgpu_native.{so,dylib,a}`
plus an unused `wgpu-native-meta/webgpu.yml` schema file. The overlay BUILD
file uses `glob(..., allow_empty = True)` so the same file works cleanly
across all four per-platform archives (each archive only carries the
`{.so, .dylib}` appropriate for its own platform; the glob is a no-op on
the other three).

#### Historical: Dawn embedding strategy

The following notes describe the original Dawn path and are retained for
historical context. Everything below was replaced by the wgpu-native swap
described above.

Dawn does not publish itself to the Bazel Central Registry, and Dawn's upstream
`BUILD.bazel` only covers Tint (the WGSL compiler). The actual WebGPU native
implementation has zero Bazel files — upstream's own `WORKSPACE.bazel` says:

> NOTE: The Bazel build is best-effort and currently only support Tint
> targets. There is no support for Dawn targets at this time.

After surveying community projects, every existing Dawn embedding goes
through CMake. We used `rules_foreign_cc`'s `cmake()` rule to drive Dawn's
upstream CMake build from inside Bazel. The full pipeline:

1. **Repository fetch**: `new_git_repository` cloned a pinned Dawn commit.
2. **Dependency population**: `patch_cmds` ran `tools/fetch_dawn_dependencies.py`
   at fetch time. This Python script was Dawn's lightweight alternative to
   depot_tools — it parsed `DEPS` files and shallow-cloned Abseil, SPIRV-Tools,
   Vulkan-Headers, etc. into `third_party/`. Unlike the build sandbox, the
   fetch phase had network access.
3. **Bazel package flattening**: `patch_cmds` stripped nested `.git` directories
   and nested `BUILD.bazel` files throughout the tree. This was critical —
   Tint's upstream Bazel support shipped 114+ `BUILD.bazel` files under
   `src/tint`, and cloned submodules like Abseil had their own too. Every
   nested `BUILD.bazel` created a Bazel package boundary that `glob(["**"])`
   stopped recursing at.
4. **Monolithic CMake build**: `cmake()` configured Dawn with
   `DAWN_BUILD_MONOLITHIC_LIBRARY=SHARED` → a single `libwebgpu_dawn.dylib`
   (~10 MB on macOS) containing Dawn native, Tint, Abseil, and SPIRV-Tools
   with hidden internal symbols. This avoided ODR/ABI clashes with other
   project dependencies.
5. **Framework linkopts**: Metal/Foundation/QuartzCore/etc. linkopts lived on
   the consuming `cc_library`, not on the `cmake()` rule.

**Why we pivoted:** clean `bazel fetch //...` on a cold GitHub Actions cache
was consistently ~1 h 45 m — the CMake test-compile alone was ~8 min, Dawn
codegen ~25 min, Abseil ~20 min, and the linker pass ~5 min. The CI budget
for the Geode backend was 30 min, so the from-source path was untenable.
Swapping to wgpu-native's prebuilt `.so` / `.dylib` releases dropped the
critical path to a ~12 MB download with no compile step.

## Proposed Architecture

### Component Overview

```
 Donner SVG pipeline                    Standalone / Game engine
 ─────────────────                      ──────────────────────────
 SVGDocument                            Application code
     |                                       |
     v                                       v
 RendererDriver ──> RendererInterface   GeoEncoder / GeoEncoder3D
                          |                  |
              +-----------+------+           |
              |           |      |           |
        FullSkiaRenderer  TinySkia  RendererGeode|
                                     |       |
                                     v       v
                              ┌──────────────────┐
                              │   donner::geode   │
                              │                   │
                              │  GeoEncoder       │  (2D drawing API)
                              │  GeoSurface       │
                              │  GeoPaint         │
                              │  GeoImage         │
                              │  GeodeDevice      │  (WebGPU lifecycle)
                              │  GeodePipeline    │  (Slug shaders)
                              │  GeodePathEncoder │  (band decomposition)
                              └──────────────────┘
```

The `donner::geode` layer is SVG-agnostic. `RendererGeode` (in `donner::svg`) is a thin adapter
that translates `RendererInterface` calls into `GeoEncoder` calls. Applications can also use
`GeoEncoder` directly for general-purpose 2D (or 3D) rendering without any SVG/DOM overhead.

### Layer Responsibilities

| Layer | Location | Responsibility |
|-------|----------|----------------|
| `RendererGeode` | `donner/svg/renderer/RendererGeode.h` | `RendererInterface` impl, state stack management, ECS cache coordination |
| `GeodeDevice` | `donner/svg/renderer/geode/GeodeDevice.h` | WebGPU device/queue lifecycle, surface management, buffer allocation |
| `GeodePipeline` | `donner/svg/renderer/geode/GeodePipeline.h` | Render pipeline objects, shader modules, bind group layouts |
| `GeodePathEncoder` | `donner/svg/renderer/geode/GeodePathEncoder.h` | Slug band decomposition: `Path` → GPU band buffers |
| `GeodeGradientEncoder` | `donner/svg/renderer/geode/GeodeGradientEncoder.h` | Gradient stop → 1D texture or SSBO encoding |
| `GeodeCompositor` | `donner/svg/renderer/geode/GeodeCompositor.h` | Isolated layers, blend modes, mask compositing via render targets |
| `GeodeTextRenderer` | `donner/svg/renderer/geode/GeodeTextRenderer.h` | Text shaping → Slug glyph submission |
| `GeodeFilterEngine` | `donner/svg/renderer/geode/GeodeFilterEngine.h` | (v2) Compute shader filter graph execution |

### General-Purpose 2D Drawing API

Geode's core is a general-purpose 2D drawing API using donner-native types. This API is
comparable in scope to tiny-skia-cpp's `Canvas` or HTML Canvas 2D, but built for GPU submission
via WebGPU rather than CPU rasterization. It serves two roles:

1. **Donner backend layer:** `RendererGeode` delegates to `GeoEncoder` for all drawing.
2. **Standalone 2D library:** Applications can use `GeoEncoder`/`GeoSurface` directly for any
   GPU-accelerated 2D rendering — UI toolkits, data visualization, creative tools, game HUDs,
   map renderers — without going through the SVG pipeline.

The namespace is `donner::geode` (not `donner::svg::geode`) to reflect that the 2D API is
SVG-independent.

```cpp
namespace donner::geode {

/// GPU drawing surface backed by a WebGPU texture.
class GeoSurface {
 public:
  /// Create a surface targeting a WebGPU texture.
  static GeoSurface FromTexture(wgpu::Texture texture, Vector2i dimensions);

  /// Create an offscreen surface with its own render target.
  static GeoSurface Offscreen(GeodeDevice& device, Vector2i dimensions);

  Vector2i dimensions() const;
  wgpu::TextureView textureView() const;
};

/// Encodes draw commands into a WebGPU command buffer.
///
/// This is the primary drawing API. It can be used standalone (without Donner's SVG pipeline)
/// for any 2D rendering workload.
class GeoEncoder {
 public:
  explicit GeoEncoder(GeodeDevice& device, GeoSurface& target);

  // --- Transform stack ---
  void setTransform(const Transform2d& transform);
  void pushTransform(const Transform2d& transform);
  void popTransform();

  // --- Clipping ---
  void pushClipRect(const Box2d& rect);
  void pushClipPath(const Path& path, FillRule fillRule);
  void popClip();

  // --- Path rendering (Slug pipeline) ---
  void fillPath(const Path& path, FillRule fillRule, const GeoPaint& paint);
  void strokePath(const Path& path, const StrokeParams& stroke, const GeoPaint& paint);

  // --- Shape convenience methods ---
  void fillRect(const Box2d& rect, const GeoPaint& paint);
  void strokeRect(const Box2d& rect, const StrokeParams& stroke, const GeoPaint& paint);
  void fillRoundedRect(const Box2d& rect, double rx, double ry, const GeoPaint& paint);
  void fillEllipse(const Box2d& bounds, const GeoPaint& paint);
  void strokeEllipse(const Box2d& bounds, const StrokeParams& stroke, const GeoPaint& paint);
  void fillCircle(const Vector2d& center, double radius, const GeoPaint& paint);
  void drawLine(const Vector2d& from, const Vector2d& to, const StrokeParams& stroke,
                const GeoPaint& paint);

  // --- Image rendering ---
  void drawImage(const GeoImage& image, const Box2d& destRect,
                 const GeoImageParams& params = {});
  void drawImageNineSlice(const GeoImage& image, const Box2d& destRect,
                          const Box2d& centerSlice, const GeoImageParams& params = {});

  // --- Text rendering (Slug glyph pipeline) ---
  void drawGlyphs(std::span<const GeoGlyph> glyphs, const GeoPaint& paint);

  // --- Compositing ---
  void pushLayer(double opacity, MixBlendMode blendMode = MixBlendMode::Normal);
  void popLayer();

  // --- Masking ---
  void pushMask();
  void transitionMaskToContent();
  void popMask();

  // --- Frame control ---
  void clear(const css::RGBA& color);

  /// Submit encoded commands.
  wgpu::CommandBuffer finish();
};

/// Paint specification using donner types.
struct GeoPaint {
  struct Solid { css::RGBA color; };
  struct LinearGradient {
    Vector2d start, end;
    std::span<const GradientStop> stops;
    GradientSpreadMethod spread = GradientSpreadMethod::Pad;
    Transform2d transform;
  };
  struct RadialGradient {
    Vector2d center;
    double radius;
    Vector2d focus;
    double focusRadius;
    std::span<const GradientStop> stops;
    GradientSpreadMethod spread = GradientSpreadMethod::Pad;
    Transform2d transform;
  };
  struct SweepGradient {
    Vector2d center;
    double startAngle;  // degrees
    double endAngle;    // degrees
    std::span<const GradientStop> stops;
    GradientSpreadMethod spread = GradientSpreadMethod::Pad;
    Transform2d transform;
  };
  struct Pattern {
    GeoImage tile;
    Box2d tileRect;
    GradientSpreadMethod spread;  // Pad, Reflect, Repeat
    Transform2d transform;
  };

  std::variant<Solid, LinearGradient, RadialGradient, SweepGradient, Pattern> shader;
  double opacity = 1.0;

  /// Convenience constructors.
  static GeoPaint FromColor(const css::RGBA& color);
  static GeoPaint FromColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
};

/// GPU-resident image handle.
class GeoImage {
 public:
  /// Upload RGBA pixel data to GPU.
  static GeoImage FromPixels(GeodeDevice& device, std::span<const uint8_t> rgba,
                             Vector2i dimensions);
  /// Upload from a Donner ImageResource.
  static GeoImage FromImageResource(GeodeDevice& device, const ImageResource& resource);

  Vector2i dimensions() const;
  wgpu::Texture texture() const;
};

/// Image drawing parameters.
struct GeoImageParams {
  double opacity = 1.0;
  MixBlendMode blendMode = MixBlendMode::Normal;
  bool pixelated = false;  // nearest-neighbor filtering
};

}  // namespace donner::geode
```

**Relationship to `RendererInterface`:** `RendererGeode` (in `donner::svg`) implements
`RendererInterface` by delegating to `GeoEncoder`. The SVG layer handles resolving paint
servers, computing transforms from the ECS, managing the rendering instance view traversal —
then calls into `GeoEncoder` for the actual GPU work. This separation means `GeoEncoder` has
no knowledge of SVG, ECS, or the DOM — it is a pure 2D drawing API.

### Slug Pipeline Detail

#### Path Encoding

`GeodePathEncoder` converts a `Path` into Slug's GPU-ready band format:

```
Path                                         GPU Buffers
┌─────────────┐    ┌──────────────┐         ┌──────────────────┐
│ commands[]   │──> │ cubicToQuad  │──┐      │ Band vertex buf  │  (bounding quads)
│ points[]     │    │ toMonotonic  │  │      │ Curve data SSBO  │  (quad ctrl pts)
│ fillRule     │    └──────────────┘  │      │ Band index SSBO  │  (2×16-bit refs)
└─────────────┘     CPU preprocessing │      └──────────────────┘
                                      └─ encode ──────^
```

1. **Band decomposition:** The path's vertical extent is divided into horizontal bands. Each
   band records which curves intersect it. Band metadata is packed as 2×16-bit per band
   (curve range start + count), following the simplified format from Slug's post-2017
   optimization that removed bidirectional ray sorting.
2. **Vertex generation:** Each band produces a bounding quad (two triangles). Each vertex stores:
   - Position in em-space (object space)
   - Outward normal vector `n̂` (scaled to represent unit polygon expansion)
   - 2×2 inverse Jacobian matrix for mapping object-space dilation back to em-space coordinates
     (handles scale, stretch, skew, and coordinate flips)
3. **Curve storage:** Control points are packed into a storage buffer. Quadratic Béziers use 3
   control points, cubics use 4. Each band references a contiguous slice of this buffer. Curves
   are stored in a single sorted order (no duplicate bidirectional lists — the band-split
   optimization was removed from Slug for good reason: shader divergence penalties outweighed
   the modest large-glyph speedup).
4. **Fragment evaluation:** For each pixel in a band quad, the fragment shader:
   a. Casts a horizontal ray from the pixel position.
   b. Finds roots of each curve's intersection with the ray using the root eligibility method.
   c. Accumulates the winding number from valid intersections.
   d. Applies the fill rule (non-zero: `|winding| > 0`; even-odd: `winding & 1`).
   e. Computes sub-pixel coverage for antialiasing at band edges.
   The root eligibility determination is the mathematical core of Slug's robustness — it
   provably avoids dropped pixels, sparkles, and streak artifacts from floating-point round-off.

The band count is adaptive — small paths (< 64px height) use a single band; large paths scale
up to prevent excessive per-pixel curve evaluation.

#### Vertex Shader: Dynamic Dilation

The vertex shader performs dynamic half-pixel dilation — the key Slug innovation that replaced
both fixed expansion constants and adaptive supersampling:

```wgsl
// Simplified 2D orthographic case (full perspective in implementation)
fn dilate_vertex(
    pos: vec2f,        // vertex position in em-space
    normal: vec2f,     // outward normal
    mvp: mat4x4f,      // model-view-projection
    viewport: vec2f,   // viewport dimensions (w, h)
) -> vec2f {
    // For orthographic: s is constant, t = 0, simplifying to:
    // d = s² / √(u² + v²)
    // where u,v are the viewport-space components of the normal
    let clip_normal = (mvp * vec4f(normal, 0.0, 0.0)).xy;
    let viewport_normal = clip_normal * viewport * 0.5;
    let len = length(viewport_normal);
    let d = 1.0 / max(len, 0.001);  // half-pixel in em-space
    return pos + normal * d;
}
```

The full perspective version implements the quadratic solution described in the Background
section. This is critical for `fillPath3D` (future 3D support) and for SVG content rendered
under CSS perspective transforms.

Each vertex also outputs em-space sampling coordinates adjusted by the inverse Jacobian:
```
em_offset = jacobian_inv * (dilated_pos - original_pos)
```
This ensures the fragment shader samples curve data in the correct coordinate space even after
dilation, handling non-uniform scaling and skew correctly.

#### Fragment Shader: Winding Number Evaluation

The fragment shader is intentionally simple — Slug's decade of production use showed that
shader simplicity (fewer branches, bounded loops, no bidirectional rays) consistently
outperforms more complex variants due to reduced divergence:

```
For each of 4 sub-pixel sample offsets (D3D-style rotated grid):
  If the sample's y is outside this band's [yMin, yMax) → skip.
  For each curve in this band's curve list:
      1. Compute ray-curve intersection roots
      2. Apply root eligibility test (filters out tangent touches,
         endpoints already counted by adjacent curves, and numerical noise)
      3. Accumulate winding number contribution (+1 or -1 per valid crossing)
  Apply fill rule to winding number → binary inside/outside.
  Set bit N of `@builtin(sample_mask)` if this sample is inside.

If the sample_mask is zero (no sample inside) → discard.
Write the full paint color (solid, gradient sample, or pattern sample) to
the color attachment; the hardware gates per-sample writes by sample_mask,
and the 4× MSAA resolve at pass end averages the surviving samples into
the 1-sample resolve target.
```

**4× MSAA with fragment-shader sample_mask.** Geode's render targets are a
(4× multisample color attachment, 1-sample resolve target) pair. The Slug
fragment shader runs once per pixel but evaluates the winding test at four
sub-pixel offsets, packing the results into `@builtin(sample_mask)` so the
hardware selects which samples receive the write. This gives fractional
edge coverage that closely matches tiny-skia's 16× supersampled
scan-converter while keeping the per-pixel winding loop count the same
order as naive single-sample shading.

The pixel-center band-Y discard is deliberately dropped in favor of a
per-sample band-Y check inside the sample_mask loop: adjacent band
fragment invocations own disjoint sample sets at band overlap boundaries,
so there is neither double coverage (from the dilated band quads
overlapping) nor a missing-coverage gap (from the earlier pixel-center
discard throwing away a fragment whose sub-pixel samples still belonged
to that band).

**Robustness guarantee:** The root eligibility method ensures deterministic winding numbers
regardless of floating-point precision. This is the one algorithm component that has remained
unchanged across Slug's entire production history — it was provably correct from the start.

#### Text Rendering

Text follows the same Slug pipeline as paths, with two key optimizations:

1. **Glyph band data is encoded once and cached.** Glyph outlines don't change — only position,
   transform, and color vary per character instance.
2. **Multi-color glyphs use independent draws, not shader loops.** Following Slug's production
   lesson: per-layer fragment shader loops waste work because most layers cover only a fraction
   of the composite glyph area. Independent draws with per-layer bounding polygons are faster.

```
ComputedTextComponent
    │
    ├── TextShaper/TextLayout  (produces positioned glyph IDs)
    │
    └── GlyphCache (ECS)
         │
         ├── GlyphBandData (per unique glyph, cached)
         │     └── band vertices + curve SSBOs + inverse Jacobian data
         │
         └── GlyphInstance[] (per character in text run)
               └── position, transform, color
               (multi-color: one instance per layer, individual bounding polygon)
```

The `GlyphCache` is stored as an ECS component on a singleton entity, keyed by (font, glyphID).
Cache invalidation piggybacks on the existing ECS dirty-flag system from the incremental
invalidation design.

#### Gradient Rendering

Gradients are evaluated in the fragment shader alongside coverage:

- **Linear/Radial/Sweep:** Gradient parameters (stops, transform, spread mode) are uploaded as
  a uniform buffer. Stop colors are packed into a 1D texture or SSBO depending on stop count.
- **The fragment shader** computes the gradient coordinate from the fragment position, samples
  the stop data, and composites with the coverage value from Slug.

This avoids a separate gradient pass — coverage and shading happen in a single fragment shader
invocation.

### State Stack Implementation

`RendererGeode` maintains a state stack mirroring the `RendererInterface` contract:

```cpp
struct GeodeState {
  Transform2d transform;
  std::optional<Box2d> clipRect;
  // Clip paths are applied via stencil buffer
  int stencilDepth = 0;
};

struct GeodeLayerStack {
  struct Layer {
    GeoSurface surface;       // Offscreen render target
    double opacity;
    MixBlendMode blendMode;
    GeodeState savedState;
  };
  SmallVector<Layer, 4> layers;
};
```

- **Transforms:** Maintained on CPU and uploaded as a uniform before each draw call. The vertex
  shader applies the combined model-view-projection matrix.
- **Clip rects:** Applied via scissor test (GPU hardware).
- **Clip paths:** Rendered into the stencil buffer using the Slug pipeline (fill with stencil
  write, then draw content with stencil test).
- **Isolated layers:** Allocate an offscreen render target, draw content into it, then composite
  back with the specified opacity and blend mode.
- **Masks:** Similar to isolated layers — render mask content to a separate target, then use it
  as an alpha/luminance mask during compositing.
- **Patterns:** Render the pattern tile into a texture, then use it as a repeating shader in
  subsequent draws.

### ECS Integration for GPU Resource Caching

Geode introduces ECS components for GPU resource lifetime management:

```cpp
/// Cached GPU band data for a resolved path. Attached to the data entity.
struct GeodePathCacheComponent {
  wgpu::Buffer bandVertices;
  wgpu::Buffer curveData;
  wgpu::Buffer bandIndex;
  uint32_t bandCount;
  uint64_t pathVersion;  // Invalidation key from Path content hash
};

/// Cached gradient texture. Attached to the gradient element entity.
struct GeodeGradientCacheComponent {
  wgpu::Texture stopTexture;       // 1D RGBA texture or SSBO
  wgpu::Buffer parameterBuffer;    // Uniform buffer for gradient params
  uint64_t gradientVersion;
};

/// Cached glyph band data. Stored on a singleton cache entity.
struct GeodeGlyphCacheComponent {
  struct Entry {
    wgpu::Buffer bandVertices;
    wgpu::Buffer curveData;
    wgpu::Buffer bandIndex;
    uint32_t bandCount;
  };
  std::unordered_map<GlyphKey, Entry> glyphs;
};

/// Cached pattern tile texture. Attached to the pattern element entity.
struct GeodePatternCacheComponent {
  wgpu::Texture tileTexture;
  Vector2i tileDimensions;
  uint64_t patternVersion;
};
```

**Cache invalidation** uses the dirty-flag system from the incremental invalidation design. When
a path, gradient, or pattern component is marked dirty, the corresponding cache component is
removed (or its version key is compared), triggering re-encoding on the next frame.

**Cache lifecycle** follows entity lifetime — when an entity is destroyed, entt automatically
destroys its cache components. GPU buffer destruction is deferred to the next frame boundary to
avoid destroying in-flight resources.

### Embeddability Design

Geode is designed to be embedded in host applications that own the GPU context:

```cpp
/// Configuration for embedding Geode in a host application.
struct GeodeEmbedConfig {
  /// Host-provided WebGPU device. If null, Geode creates its own.
  wgpu::Device device = nullptr;

  /// Host-provided queue. Required if device is provided.
  wgpu::Queue queue = nullptr;

  /// Target texture format. Must match the texture passed to beginFrame.
  wgpu::TextureFormat format = wgpu::TextureFormat::BGRA8Unorm;

  /// Maximum texture dimension for offscreen targets (layers, patterns, masks).
  uint32_t maxOffscreenDimension = 4096;

  /// If true, Geode inserts GPU timestamp queries for performance profiling.
  bool enableTimestamps = false;
};

class RendererGeode : public RendererInterface {
 public:
  /// Create with Geode-managed device (standalone mode).
  explicit RendererGeode(bool verbose = false);

  /// Create with host-provided device (embedded mode).
  explicit RendererGeode(const GeodeEmbedConfig& config);

  /// Render into a host-provided texture instead of an internal surface.
  /// Call this between beginFrame() and the first draw call.
  void setTargetTexture(wgpu::Texture texture);
};
```

**Integration patterns:**

1. **Standalone:** Geode creates and owns a `wgpu::Device`. `takeSnapshot()` reads back pixels
   via a staging buffer. Used for tests and CLI tooling.
2. **Embedded (texture target):** Host provides device + queue + target texture. Geode submits
   command buffers to the host queue. Host composites Geode output with other content.
3. **Embedded (render pass injection):** Future extension — host provides an active render pass
   encoder, and Geode appends draw calls directly. Avoids an extra render target copy for
   overlay-style SVG rendering.

## GPU Resource Budget

| Resource | Typical Size | Notes |
|----------|-------------|-------|
| Band vertex buffer (per path) | 1–8 KB | Scales with path complexity and band count |
| Curve data SSBO (per path) | 0.5–4 KB | Proportional to curve count |
| Glyph cache (per unique glyph) | ~0.5 KB | Amortized across text runs |
| Gradient stop texture | 256 B–1 KB | 1D texture, typically ≤64 stops |
| Offscreen render target | W×H×4 B | One per isolated layer / mask / pattern in flight |
| Stencil buffer | W×H×1 B | Shared across clip operations |

For a typical SVG document with ~100 paths and a few text runs, total GPU memory is in the
low single-digit MB range, dominated by render targets for compositing layers.

## Type Refactoring (Pre-Phase 1)

Before Geode implementation begins, core donner types need refactoring to prepare for 3D
support and to improve the path API for GPU rendering. These changes affect the entire codebase
and should land as standalone PRs before Geode-specific code.

### Rename: `Transform` → `Transform2`

| Current | New | References | Files |
|---------|-----|-----------|-------|
| `Transform2d` | `Transform2d` | ~514 | ~57 |
| `Transformf` | `Transform2f` | ~2 | ~2 |
| `Transform<T>` | `Transform2<T>` | (template) | 1 |

**Why now:** Makes room for `Transform3<T>` / `Transform3d` (4×4 matrix) without ambiguity.
The current name `Transform` implies generality but is inherently 2D (3×3 affine matrix, 6
parameters).

**Migration strategy:** Mechanical rename via `sed`/IDE refactor. Keep `using Transform2d = Transform2d`
compatibility aliases in `Transform.h` transitionally for downstream consumers, then remove.

### Rename: `Box` → `Box2`

| Current | New | References | Files |
|---------|-----|-----------|-------|
| `Box2d` | `Box2d` | ~516 | ~76 |
| `Box<T>` | `Box2<T>` | (template) | 1 |

**Why now:** Same reasoning as Transform. `Box` uses `Vector2<T>` corners internally — the 3D
equivalent (`Box3<T>` / AABB) would use `Vector3<T>` corners and add a `depth()` method.

**Migration strategy:** Same as Transform — mechanical rename with temporary alias.

### `Vector2` — No rename needed

`Vector2` is already explicitly dimensioned. Add `Vector3<T>` alongside it when 3D work begins
(post-v1). No action needed now.

### Refactor: `PathSpline` → `Path` + `PathBuilder` (DONE)

This refactoring is complete. `PathSpline` has been replaced by `Path` (immutable, in
`donner/base/Path.h`) and `PathBuilder` (mutable builder). `Path` is constructed via
`PathBuilder::build()` and lives in the `donner` namespace.

**New types:**

```cpp
namespace donner {

/// Immutable 2D vector path. Once constructed, a Path is thread-safe and suitable for caching.
class Path {
 public:
  /// Command types in the path.
  enum class Verb {
    MoveTo,     // 1 point
    LineTo,     // 1 point
    QuadTo,     // 2 points (control + end) — NEW
    CurveTo,    // 3 points (control1 + control2 + end)
    ClosePath,  // 0 points
  };

  // --- Accessors ---
  std::span<const Vector2d> points() const;
  std::span<const Command> commands() const;
  bool empty() const;
  size_t verbCount() const;

  // --- Geometric queries ---
  Box2d bounds() const;
  Box2d transformedBounds(const Transform2d& transform) const;
  Box2d strokeMiterBounds(double strokeWidth, double miterLimit) const;
  double pathLength() const;
  PointOnPath pointAtArcLength(double distance) const;
  Vector2d pointAt(size_t segmentIndex, double t) const;
  Vector2d tangentAt(size_t segmentIndex, double t) const;

  // --- Hit testing ---
  bool isInside(const Vector2d& point, FillRule fillRule) const;
  bool isOnPath(const Vector2d& point, double tolerance) const;

  // --- Conversion ---
  /// Convert all cubic curves to quadratic approximations within the given tolerance.
  /// Critical for Slug pipeline efficiency — quadratic root-finding is cheaper than cubic.
  Path cubicToQuadratic(double tolerance = 0.1) const;

  /// Split all curves at Y-extrema so each segment is monotonic in Y.
  /// Required for Slug band decomposition.
  Path toMonotonic() const;

  /// Flatten all curves to line segments within the given tolerance.
  Path flatten(double tolerance = 0.25) const;

  /// Expand a stroked path into a filled outline.
  Path strokeToFill(const StrokeParams& stroke) const;

  // --- Iteration ---
  /// Iterate over path segments with a visitor.
  template <typename Visitor>
  void forEach(Visitor&& visitor) const;
};

/// Mutable builder for constructing immutable Path objects.
class PathBuilder {
 public:
  PathBuilder() = default;

  // --- Construction ---
  PathBuilder& moveTo(const Vector2d& point);
  PathBuilder& lineTo(const Vector2d& point);
  PathBuilder& quadTo(const Vector2d& control, const Vector2d& end);          // NEW
  PathBuilder& curveTo(const Vector2d& c1, const Vector2d& c2, const Vector2d& end);
  PathBuilder& arcTo(const Vector2d& radius, double rotation,
                     bool largeArc, bool sweep, const Vector2d& end);
  PathBuilder& closePath();

  // --- Shape helpers ---
  PathBuilder& addRect(const Box2d& rect);
  PathBuilder& addRoundedRect(const Box2d& rect, double rx, double ry);
  PathBuilder& addEllipse(const Box2d& bounds);
  PathBuilder& addCircle(const Vector2d& center, double radius);
  PathBuilder& addPath(const Path& path);

  // --- State ---
  Vector2d currentPoint() const;
  bool empty() const;

  /// Build the immutable Path. The builder is reset after this call.
  Path build();
};

}  // namespace donner
```

**Key changes from the old `PathSpline`:**

| Change | Rationale |
|--------|-----------|
| **Split into `Path` (immutable) + `PathBuilder` (mutable)** | Thread safety, cacheable, clear ownership semantics. Cached `GeodePathCacheComponent` holds a `Path` that never changes underneath it. |
| **Add `QuadTo` verb** | Slug evaluates quadratic curves more efficiently than cubics (quadratic root-finding vs. cubic). SVG `<path>` has Q/q commands. Currently these are converted to cubics unnecessarily. |
| **`cubicToQuadratic()` conversion** | Slug's fragment shader solves for ray-curve intersections. Quadratic = one `f32` square root; cubic = Cardano's formula with potential numerical issues. Converting to quadratics where possible reduces fragment shader cost and improves numerical stability. |
| **`toMonotonic()` splitting** | Band decomposition requires curves that are monotonic in Y (each curve crosses any horizontal line at most once). Without this, band assignment is ambiguous. |
| **`flatten()` method** | Useful for stroke expansion fallback and hit testing. Exists internally as `SubdivideAndMeasureCubic` but not exposed. |
| **`strokeToFill()` method** | Replaces the open question about stroke rendering strategy — CPU stroke expansion as the initial approach, GPU expansion as future optimization. |
| **`forEach` visitor** | Clean iteration without exposing internal indices. Useful for `GeodePathEncoder`. |

**Cubic-to-quadratic decomposition algorithm:**

The standard approach decomposes a cubic Bézier into a sequence of quadratic Béziers within a
given tolerance. For each cubic segment:

1. Check if the cubic is "close enough" to a quadratic by measuring the distance between the
   cubic's control points and the best-fit quadratic's control points.
2. If within tolerance, emit the approximating quadratic.
3. Otherwise, subdivide the cubic at `t = 0.5` (de Casteljau) and recurse on each half.

Typical SVG paths convert to 2–4 quadratics per cubic. The tolerance parameter controls
quality vs. curve count — `0.1` (em-space units) is a good default for text-size content;
larger paths may want tighter tolerance.

**Monotonic splitting algorithm:**

For each quadratic or cubic curve:

1. Find the parameter values `t` where `dy/dt = 0` (Y-extrema).
   - Quadratic: solve linear equation → at most 1 split.
   - Cubic: solve quadratic equation → at most 2 splits.
2. Split the curve at each extremum using de Casteljau subdivision.
3. Each resulting sub-curve is guaranteed to be monotonic in Y.

This is required for Slug band decomposition — a monotonic curve intersects any horizontal
band boundary at most once, which simplifies the curve-to-band assignment and ensures correct
winding number accumulation.

**Migration status:** Complete. `PathSpline` has been removed. All callers now use `Path`
and `PathBuilder`. `RendererInterface` methods use `Path`.

### New Bézier Utilities

These should live in `donner/base/` (or `donner/geode/base/` if extracted) as free functions,
usable independently of `Path`:

```cpp
namespace donner {

// --- De Casteljau subdivision ---

/// Split a quadratic Bézier at parameter t. Returns {left, right} as point arrays.
std::pair<std::array<Vector2d, 3>, std::array<Vector2d, 3>>
SplitQuadratic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, double t);

/// Split a cubic Bézier at parameter t.
std::pair<std::array<Vector2d, 4>, std::array<Vector2d, 4>>
SplitCubic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
           const Vector2d& p3, double t);

// --- Cubic-to-quadratic approximation ---

/// Approximate a cubic Bézier as a sequence of quadratics within tolerance.
/// Appends quadratic control point pairs to `out`.
void ApproximateCubicWithQuadratics(
    const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, const Vector2d& p3,
    double tolerance, SmallVector<Vector2d, 8>& out);

// --- Monotonic splitting ---

/// Find parameter values where dy/dt = 0 for a quadratic.
SmallVector<double, 1> QuadraticYExtrema(
    const Vector2d& p0, const Vector2d& p1, const Vector2d& p2);

/// Find parameter values where dy/dt = 0 for a cubic.
SmallVector<double, 2> CubicYExtrema(
    const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, const Vector2d& p3);

// --- Bounding boxes ---

/// Tight bounding box for a quadratic Bézier (evaluates at extrema, not just control points).
Box2d QuadraticBounds(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2);

/// Tight bounding box for a cubic Bézier.
Box2d CubicBounds(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                  const Vector2d& p3);

// --- Evaluation ---

/// Evaluate a quadratic Bézier at parameter t.
Vector2d EvalQuadratic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, double t);

/// Evaluate a cubic Bézier at parameter t.
Vector2d EvalCubic(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                   const Vector2d& p3, double t);

}  // namespace donner
```

### Refactoring Order

These should land as separate PRs in dependency order:

1. **`Transform` → `Transform2`** — Mechanical rename, compatibility alias. No behavioral change.
2. **`Box` → `Box2`** — Mechanical rename, compatibility alias. No behavioral change.
3. **New Bézier utilities** — Pure additions, no existing code changes. Add with unit tests and
   fuzz tests (per project conventions).
4. **`Path` + `PathBuilder`** — Done. New types replaced `PathSpline`.
5. **`RendererInterface` migration** — Done. `PathShape` and related types use `Path`.
6. **Caller migration** — Done. All callers migrated.
7. **Remove `PathSpline`** — Done. `PathSpline` removed.

PRs 1–3 can proceed in parallel. PR 4 depends on 3. PR 5 depends on 4. PRs 6–7 are follow-up
cleanup.

## Implementation Plan

### Phase 0: Type Refactoring (pre-Geode)

- [x] Rename `Transform<T>` → `Transform2<T>`, `Transform2d` → `Transform2d` with compatibility
  aliases.
- [x] Rename `Box<T>` → `Box2<T>`, `Box2d` → `Box2d` with compatibility aliases.
- [x] Implement Bézier utility functions: `SplitQuadratic`, `SplitCubic`,
  `ApproximateCubicWithQuadratics`, `QuadraticYExtrema`, `CubicYExtrema`, `QuadraticBounds`,
  `CubicBounds`, `EvalQuadratic`, `EvalCubic`.
  - [ ] Unit tests for all utilities.
  - [ ] Fuzz tests for cubic-to-quadratic approximation and monotonic splitting.
- [x] Implement `Path` (immutable) and `PathBuilder` (mutable), replacing `PathSpline`.
  - [x] Add `QuadTo` verb support.
  - [x] Implement `cubicToQuadratic()`.
  - [x] Implement `toMonotonic()`.
  - [x] Implement `flatten()`.
  - [x] Implement `strokeToFill()` — flattens curves, offsets each segment
    by width/2, applies cap/join with miter-limit fallback to bevel.
    Produces two same-winding closed contours per closed subpath, so
    callers must fill with `EvenOdd` to get the expected hollow ring.
- [x] Migrate `RendererInterface` from `PathSpline` to `Path`.
- [x] Migrate remaining callers, remove `PathSpline`.

### Phase 1: Foundation and Path Rendering

> **Note (2026-04-17):** The Dawn `rules_foreign_cc` + CMake vendoring
> described in this checklist was the original Phase 1 plan and is what
> actually shipped in #484. It was later superseded by #510, which swapped
> Dawn-from-source for prebuilt `wgpu-native` archives. See the "Bazel
> vendoring strategy (wgpu-native)" section under Background for the
> current authoritative vendoring design. The historical Dawn content
> below is retained unchanged for context.

- [x] Vendor Dawn (WebGPU) as a third-party dependency with Bazel build. **(#484)**
  - Uses `rules_foreign_cc`'s `cmake()` rule to drive Dawn's upstream CMake
    build (Dawn has no usable native Bazel support — the root `BUILD.bazel`
    only exposes Tint).
  - `new_git_repository` with `patch_cmds` runs
    `tools/fetch_dawn_dependencies.py` at fetch time (network is available
    there but not in the sandbox), then strips nested `.git` dirs and
    nested `BUILD.bazel` files so Bazel's `glob(["**"])` sees every
    submodule as regular sources.
  - CMake builds a single monolithic shared library
    (`DAWN_BUILD_MONOLITHIC_LIBRARY=SHARED`, ~10 MB dylib) that hides
    Abseil/Tint/SPIRV-Tools symbols internally to avoid ODR/ABI clashes.
  - Platform linkopts (`-framework Metal` etc. on macOS) live on the
    consuming `cc_library`, NOT on the `cmake()` rule — adding them to
    `cmake()` makes `rules_foreign_cc` apply them during Dawn's own
    compiler-test step and breaks the build.
  - Clean build takes ~4.5 min on macOS via Bazel's parallelism.
  - Gated behind `--//donner/svg/renderer/geode:enable_geode=true`
    (default: false) so existing CI is unaffected.
- [x] Implement `GeodeDevice`: headless device/queue factory.
  - Uses `dawn::native::Instance::EnumerateAdapters()` + `wgpu::Adapter(ptr)`
    (adds a ref — do NOT use `Acquire` here, which steals the ref and causes
    double-free when the `dawn::native::Adapter` vector destructs).
  - No window system integration — purely offscreen rendering into textures.
  - `DeviceLostCallback` is intentionally not set — Dawn fires it during
    normal device destruction which interacts poorly with gtest teardown.
  - **4 tests passing on macOS Metal**, including end-to-end clear+readback
    verifying that the first pixel of a cleared texture is `(255, 0, 0, 255)`.
- [x] Implement Slug vertex shader: MVP transform, dynamic half-pixel
  dilation. **(WGSL in `shaders/slug_fill.wgsl`, compiles via Tint.)**
- [x] Implement Slug fragment shader: ray-curve intersection, winding
  number, coverage. **(compiles via Tint; end-to-end rendering not yet
  wired up.)**
  - [x] Non-zero fill rule.
  - [x] Even-odd fill rule.
- [x] Implement `GeodePathEncoder`: `Path` → Slug band decomposition. **(commit `e42f3f75`)**
- [x] Implement `GeoEncoder` core: transform stack, solid color fill, path
  rendering. **(commit `ddbcda6b`)**
- [x] Implement `RendererGeode` skeleton: `beginFrame`/`endFrame`,
  `setTransform`, `drawPath` with solid fill.
- [x] Add Bazel `--config=geode` backend selection.
- [x] Run basic renderer tests (solid-fill SVGs) against golden images —
  `renderer_geode_golden_tests` with per-backend goldens under
  `testdata/golden/geode/`; 5/5 green on macOS Metal.
- [x] Linux CI headless Vulkan via Mesa `llvmpipe`. **(Switched from
  SwiftShader: Ubuntu's `mesa-vulkan-drivers` apt package ships llvmpipe,
  a maintained pure-software Vulkan ICD. No vendoring/CMake work
  required. Added as experimental `linux-geode` CI job —
  `continue-on-error: true` until first run proves it out.)**

### Phase 2: Complete SVG Painting

- [x] Stroke rendering via `Path::strokeToFill()` → Slug fill pipeline.
  `RendererGeode::drawPath` expands the stroked outline on the CPU via
  `Path::strokeToFill` and fills it through the existing Slug pipeline.
  The fill rule is chosen **per source**: open source paths produce a
  single-subpath result rendered with `NonZero` (overlapping start/end
  caps + inside-miter shortcuts in `emitJoin` can self-intersect and
  EvenOdd would drop whole segments); closed source paths produce a
  two-subpath result (outer + inner, same winding) rendered with
  `EvenOdd` to get the hollow ring. `drawPath` counts `MoveTo` verbs in
  the result path to select the rule.
    * **Dashes, dashoffset, pathLength** — plumbed through
      `Path::strokeToFill`'s dash splitter. Each dash is stroked as its
      own open sub-polyline with butt caps. The final dash is truncated
      at `totalArc` for closed subpaths (the earlier wrap-around-stitch
      path overlapped with the first iteration's dash at arc 0 and
      EvenOdd cancellation produced a visible gap at the seam — now
      fixed, matches tiny-skia).
    * **Round / square / butt caps** — handled by `emitCap`. SVG 2 §11.4
      zero-length subpath caps (square → axis-aligned square, round →
      full circle, butt → nothing) detected up front in `strokeSubpath`
      before the normal segment-normal loop.
    * **Sharp concave corners on open subpaths** — `emitJoin`'s inside-
      turn branch emits the true offset-line intersection (the miter
      point) so the resulting polygon is geometrically clean.
    * **Curved flattened strokes on closed subpaths** — rounded rects,
      ellipses, and quadratic curves (`rect2`, `ellipse1`, `skew1`,
      `quadbezier1`) all pass after the strokeToFill regressions landed
      earlier in Phase 2.
  Outstanding: `painting/stroke-linejoin/miter` still shows a ~2-pixel
  offset at the bevel-fallback corner tip, marked `disableBackend(Geode)`
  with a TODO to align `emitJoin`'s outside-turn branch with tiny-skia's
  reference.
- [🚧] Implement `GeodeGradientEncoder`: linear, radial, and sweep gradients.
  - [x] **Linear gradients (Phase 2E).** Shipped as a sibling pipeline
    (`GeodeGradientPipeline`) + fragment shader
    (`shaders/slug_gradient.wgsl`) + `GeoEncoder::fillPathLinearGradient`.
    Supports both `userSpaceOnUse` and `objectBoundingBox` units, the
    `gradientTransform` attribute, and all three spread modes. Stops are
    baked into the per-draw uniform (cap: 16 stops — follow-up is a
    texture-based stop lookup via `GeodeGradientCacheComponent`). The
    `RendererGeode` fill/stroke dispatch shares a single code path
    (`drawPaintedPathAgainst`) so gradient strokes reuse the *original*
    path bounds for gradient coordinate resolution, matching the SVG spec
    and the other backends. Golden coverage:
    `linear_gradient_basic.svg` (objectBoundingBox / horizontal / pad),
    `linear_gradient_userspace.svg` (userSpaceOnUse + rotate
    gradientTransform + 3-stop),
    `linear_gradient_spread.svg` (three rects exercising
    pad / reflect / repeat side by side),
    `linear_gradient_stroke.svg` (stroked rect outline gradient-filled).
    Unit-test coverage in `GeoEncoder_tests.cc` exercises the pipeline
    directly (`FillLinearGradientUserSpace`, `FillLinearGradientRepeat`).
  - [x] **Radial gradients (Phase 2F).** Shipped by extending
    `slug_gradient.wgsl` with a `gradientKind` discriminator (linear vs.
    radial) and a small uniform-layout grow (`GradientUniforms` is now
    480 bytes, up from 464). The fragment stage forks into one of two
    `t` derivations before reaching the shared `apply_spread` /
    `sample_stops` path:
      * **linear** — projects the gradient-space sample onto the
        `(start, end)` axis (unchanged from Phase 2E).
      * **radial** — solves the SVG 2 / Canvas two-circle quadratic
        `|e − t·d|² = (Fr + t·Dr)²` for `t`, taking the root whose
        corresponding radius `Fr + t·Dr` is non-negative. Reduces to the
        closed form `|P − C| / R` when the focal point coincides with
        the center and `fr == 0`. See `radial_t()` in
        `shaders/slug_gradient.wgsl` for the full derivation.
    `RendererGeode::resolveRadialGradientParams` shares its frame /
    transform / stop-list helpers with the linear resolver via the new
    `resolveGradientFrame` and `buildGradientStops` factor-outs, so the
    only branch-specific code is the geometry resolution and the new
    `geode::RadialGradientParams` struct. Both fill and stroke routes
    work — the existing `drawPaintedPathAgainst` dispatch tries linear
    first, then radial, then falls back to the reference's solid
    fallback color. Golden coverage:
    `radial_gradient_basic.svg` (objectBoundingBox / concentric / pad),
    `radial_gradient_userspace.svg` (userSpaceOnUse + anisotropic
    gradientTransform + 3-stop),
    `radial_gradient_focal.svg` (off-center focal point exercising the
    general two-circle quadratic),
    `radial_gradient_spread.svg` (pad / reflect / repeat side by side
    on a 30%-radius gradient),
    `radial_gradient_stroke.svg` (stroked rect outline radial-filled).
    Unit-test coverage in `GeoEncoder_tests.cc` exercises the encoder
    directly (`FillRadialGradientConcentric`, `FillRadialGradientFocal`).
  - [ ] Sweep / conic gradients (Phase 2F-followup). The donner SVG
    parser does not yet expose a `<conicGradient>` element nor a
    `ComputedSweepGradientComponent`, so there is nothing for the
    renderer to dispatch on. The shader is structured to take a third
    branch when the parser side lands: add a `kGradientSweep` enum
    value, drop a `sweep_t()` function next to `radial_t()` (one
    `atan2` over the gradient-space delta), and wire a sibling
    resolver in `RendererGeode`. Tracking issue / parser support is
    a prerequisite.
  - [x] Spread modes: pad, reflect, repeat (covered by both linear and
    radial paths).
- [x] Implement pattern tile rendering: render tile to offscreen texture, sample as repeating
  shader. The Slug fill shader gained a `paintMode` uniform (0 = solid, 1 = pattern), a
  `patternFromPath` affine transform, and texture + sampler bindings. `beginPatternTile`
  allocates an offscreen `wgpu::Texture`, finishes the outer `GeoEncoder`, and redirects
  subsequent draws into a nested `GeoEncoder` on the tile. `endPatternTile` finishes the
  tile encoder, stashes the texture as the fill or stroke paint, and creates a fresh outer
  encoder with `setLoadPreserve()` so earlier outer content is retained. Pattern fills
  reuse the winding-number coverage path, so non-rectangular pattern fills (e.g., pattern
  inside a triangle) work transparently. Goldens: `geode_pattern_solid`, `_checker`,
  `_offset`, `_nonrect`.
- [ ] Implement `drawRect` and `drawEllipse` optimized paths (skip band decomposition for
  axis-aligned primitives).
- [x] Implement `drawImage`: textured quad with opacity and filtering.
  Shipped via `GeodeImagePipeline` + `GeodeTextureEncoder` (see
  implementation-status note above). Unit-tested in
  `geo_encoder_tests` and `renderer_geode_tests`; golden tests in
  `renderer_geode_golden_tests` (`image_data_url_pixelated.svg`,
  `image_data_url_opacity.svg`). The same `GeodeTextureEncoder`
  helpers will be reused by Phase 2H pattern tile sampling.
- [ ] Implement `GeodeGradientCacheComponent` for ECS gradient caching.

### Phase 3: Compositing and Clipping

- [x] Implement scissor-based clip rect.
- [x] **Phase 3a: convex 4-vertex clip polygon.** For non-axis-aligned
  ancestor transforms (e.g., a skewed or rotated `<symbol>` / `<svg>`
  viewport), the rectangular scissor can only describe the AABB of the
  transformed viewport, not the true parallelogram. `GeoEncoder` now
  accepts `setClipPolygon(corners[4])` / `clearClipPolygon()`, uploads
  4 inward half-planes through the `Uniforms` / `GradientUniforms`
  blocks, and the fragment shader ANDs a per-sample half-plane test
  into its `@builtin(sample_mask)` so the clip integrates with the 4×
  MSAA coverage path. `RendererGeode::pushClip` detects non-axis-
  aligned transforms via the 2×2 linear part and pushes the 4
  transformed corners alongside the existing scissor AABB. Re-enables
  `structure/symbol/with-transform-on-use{,-no-size}` on the Geode
  resvg suite. Nested polygon clips (unusual) fall back to the
  topmost polygon — no in-shader polygon-intersection pass yet.
- [x] **Phase 3b: path clipping via R8 mask texture.** Arbitrary SVG
  `clip-path` references are now honoured. `GeoEncoder` gains a
  `beginMaskPass` / `fillPathIntoMask` / `endMaskPass` /
  `setClipMask` / `clearClipMask` API. A new `GeodeMaskPipeline`
  + `shaders/slug_mask.wgsl` renders clip paths into a 4× MSAA
  R8Unorm target that resolves to a 1-sample R8Unorm texture; the
  main fill + gradient pipelines gain an extra texture+sampler
  binding and their fragment shaders sample `mask.r` at each pixel
  center and multiply it into the output colour. A 1x1 dummy R8
  (value 0xFF) is bound when no clip is active so the bind group
  layout stays stable. `RendererGeode::pushClip` allocates mask
  texture(s) per clip, walks `clip.clipPaths` applying the
  `clipPathUnitsTransform × parentFromEntity × currentTransform`
  chain (matches `RendererTinySkia`), and stashes the outermost
  resolve view on the clip stack entry so `updateEncoderScissor`
  can bind the topmost mask. Multiple clip paths within a single
  layer union via `BlendOperation::Max` on the R channel.

  **Nested clip-path support (follow-up, also done):** The mask
  pipeline itself accepts a clip mask input — `slug_mask.wgsl`
  samples `clipMaskTexture.r` at the pixel center and multiplies it
  into the shape's coverage output. `RendererGeode::pushClip`
  partitions `clip.clipPaths` into contiguous layer runs, then
  renders runs bottom-up (deepest first). Each outer layer binds
  the previously-rendered deeper layer's mask as its input clip,
  so every outer-layer shape is intersected with the deeper union
  and `BlendOperation::Max` unions the outer shapes on top —
  matching `RendererTinySkia::pushClip`'s recursive
  `buildLayerMask`. Nested `<g>` clips (two separate `pushClip`
  calls stacking) are handled by seeding the deepest layer's input
  clip with the topmost existing clip stack entry's mask view,
  intersecting the new clip with the active ancestor clip as it's
  rendered. Unlocks the entire `masking/clipPath` +
  `masking/clip` + `masking/clip-rule` category plus all
  cross-category `*-on-clipPath` tests. Session delta: 596 → 636
  passing on `resvg_test_suite_geode_text`.
- [ ] Implement stencil-based clip path — superseded by Phase 3b
  texture-mask clipping, which uses a resolved R8 mask sampled by the
  main fill / gradient fragment shaders. Stencil would still be a
  valid optimisation (skip the sample + the offscreen pass for simple
  clip rects), but is no longer on the critical path.
- [x] Implement `pushIsolatedLayer`/`popIsolatedLayer`: offscreen
  render target allocation + opacity compositing. (Phase 2 landing.)
  - [ ] Blend mode fragment shader (all 28 SVG/CSS blend modes).
    Still pending — `popIsolatedLayer` does a plain premultiplied
    source-over today. `painting/mix-blend-mode` + `painting/isolation`
    remain category-gated.
- [x] **Phase 3c: `<mask>` compositing via luminance blit.** The
  existing `GeodeImagePipeline` is extended with a second texture
  binding (luminance mask) + `maskMode` / `applyMaskBounds` /
  `maskBounds` uniforms; a 1x1 dummy mask is bound for normal
  `drawImage` / `blitFullTarget` calls so layout stays stable. The
  fragment shader computes `0.2126·R_pm + 0.7152·G_pm + 0.0722·B_pm`
  (which equals `luminance(demult) · alpha` for premultiplied input)
  and multiplies it into the output colour, matching tiny-skia's
  `Mask::fromPixmap(Luminance)`. `RendererGeode::pushMask` allocates
  two offscreen texture pairs (mask capture + masked content),
  redirects the encoder into the first pair, and saves the outer
  target + the `currentTransform` as the mask-bounds reference frame.
  `transitionMaskToContent` swaps the encoder into the content pair.
  `popMask` composites the pair back onto the restored parent via
  `GeoEncoder::blitFullTargetMasked`, lifting the raw mask-bounds
  rect into device-pixel space through the saved transform so
  `maskUnits=userSpaceOnUse` and percent-sized bounds render
  correctly. Unlocks the entire `masking/mask` category (31/31
  tests passing). Session delta: 636 → 666 passing on
  `resvg_test_suite_geode_text`.
- [ ] Implement `GeodePatternCacheComponent` for ECS pattern caching.

### Phase 4: Text Rendering

- [ ] Implement `GeodeTextRenderer`: glyph outline extraction from font data → Slug encoding.
- [ ] Implement `GeodeGlyphCacheComponent`: per-glyph band data cache in ECS.
- [ ] Implement instanced glyph rendering: per-character position/transform/color.
- [ ] Integrate with `TextShaper` (text-full config) and `TextLayout` (base config).
- [ ] Implement `drawText` on `RendererGeode`.
- [ ] Run text-related renderer tests across base, text-full, and Geode configs.

### Phase 5: ECS Cache Integration and Performance

- [ ] Implement `GeodePathCacheComponent`: cache encoded band data on path entities.
- [ ] Implement cache invalidation via dirty flags from incremental invalidation system.
- [ ] Implement deferred GPU resource destruction (frame-boundary cleanup).
- [ ] Implement GPU timestamp profiling (behind `enableTimestamps` flag).
- [ ] Performance benchmarking: compare against the archived full-Skia renderer and RendererTinySkia on the
  resvg test suite and complex real-world SVGs.
- [ ] Optimize: batch draw calls for paths sharing the same pipeline state.

### Phase 5b: Full Test-Suite Parity with tiny-skia

Today Geode runs a curated `renderer_geode_golden_tests` target (33 SVGs as of Phase 2)
against per-backend goldens, while the main `renderer_tests` target (87 SVGs) and the
resvg test suite (~600 SVGs) are gated out of the Geode build via `target_compatible_with`.
This gap exists because:
  - Geode's Slug-based rasterization has sub-pixel AA differences from tiny-skia's
    supersampling that would fail strict-identity comparisons on every edge pixel.
  - Some features (text, clipping, masks, filter layers) are still stubbed in
    `RendererGeode` as of Phase 2, so most resvg tests would fail outright.

The target is to lift `target_compatible_with = [skia, tiny_skia]` from the main
test targets and run them through Geode too. **Per-backend golden files are
treated as a bug smell**: the preferred override is a threshold bump with a
TODO explaining the divergence source, not a separate `golden/geode/*.png`
capture. The `ImageComparisonParams` override table in
`Renderer_tests.cc::geodeOverrides()` carries those per-test widenings, same
pattern as the resvg suite's `getTestsWithPrefix` map.

- [x] **Unblock the main renderer golden suite for Geode.** `:renderer_tests`
  now runs under `--config=geode` against the shared tiny-skia-authored
  goldens. Sub-pixel AA divergences are absorbed by a widened default
  threshold (`kGeodeDefaultMaxMismatchedPixels = 2000`), and per-test
  exceptions live in `geodeOverrides()` with TODO comments describing the
  root cause to investigate. Filter-dependent tests (e.g., `feImage`)
  auto-skip via `requireFeature(FilterEffects)` until Phase 7 lands.
- [ ] **Root-cause the current `geodeOverrides()` entries and shrink the
  table.** Each TODO in the override map corresponds to a real Geode
  divergence that should be fixed (thin-stroke AA, conical radial solver,
  nested-SVG clip). The long-term goal is an empty `geodeOverrides()` map.
- [x] **Unblock the resvg test suite for Geode.** A `geode` variant is
  now live on `donner/svg/renderer/tests:resvg_test_suite`. The category
  auto-gate in `resvg_test_suite.cc::geodeCategoryGate` cleanly skips
  entire directories (`filters/*`, `text/*`, `masking/{clip,clipPath,
  mask,clip-rule}`, `painting/{marker,mix-blend-mode,isolation}`) via
  `requireFeature` / `disableBackend` on Geode only so Skia / TinySkia
  continue to run the full suite at their strict thresholds. Per-
  filename cross-category gates (`*-on-text*`, `*-on-tspan*`,
  `*-on-marker*`, `*-on-clipPath*`, `bBox-impact`, etc.) catch tests
  that embed blocked features without living in the blocked categories.
  The `widenThresholdForGeode` helper raises the per-pixel threshold
  only when `kActiveIsGeode` is true — used for a handful of
  `structure/image/preserveAspectRatio` tests where Geode's 4× MSAA
  quantisation drifts within ~10% of tiny-skia's 16× supersample but
  trips the default 2% cutoff.
- [x] **Close the feature gaps that show up as systematic resvg failures.**
  Most of the remaining failures after the category gates are genuine
  Geode bugs that landed as part of the MSAA PR (#504):
    * Nested isolated-layer blit double-premult (fixed via a
      `sourceIsPremultiplied` flag on `GeodeTextureEncoder::QuadParams`
      → `image_blit.wgsl` skips its default straight-to-premult
      conversion for layer textures).
    * Gradient stop interpolation in straight alpha instead of
      premult (fixed in `GeoEncoder::populateSharedGradientUniforms`
      + `slug_gradient.wgsl` fragment stage).
    * Closed-subpath dash wrap-around producing double-coverage at
      the seam (fixed in `Path::strokeDashedSubpath` — truncate at
      `totalArc` and let the first dash cover the head region).
    * Open-subpath stroke fill rule (now per-source, selected by
      counting `MoveTo` verbs in the `strokeToFill` result).
    * Zero-length subpath stroke caps (SVG 2 §11.4 shapes emitted
      directly from `strokeSubpath`).
  Phase 3a polygon clipping unblocked
  `structure/symbol/with-transform-on-use{,-no-size}` — the remaining
  per-file TODOs are `structure/image/preserveAspectRatio=xMaxYMax-
  slice-on-svg` (polygon clip edge AA fringes 4 pixels past the 100-px
  max — a follow-up, not a functional gap) and
  `painting/stroke-linejoin/miter` (bevel-fallback corner drift).
- [x] **Track the pass-rate delta between Geode and the full-Skia renderer.**
  After #504, `resvg_test_suite_geode_text` is **596 passing / 0
  failing / 765 skipped via feature gates** on top of the category
  auto-gate infrastructure. Skia and tiny-skia backends run the same
  set of categories at strict thresholds and are unchanged. The
  remaining Geode-specific skips are the four per-test TODOs listed
  above plus the whole-category gates for features not yet
  implemented (filters, text, clipping, markers, mix-blend-mode,
  isolation).

### Phase 6: Embeddability

- [ ] Implement `GeodeEmbedConfig`: host-provided device/queue/format.
- [ ] Implement `setTargetTexture` for rendering into host-owned textures.
- [ ] Document embedding API and provide example integration code.
- [ ] Test embedded mode with a minimal SDL/GLFW host application.

### Phase 7 (v2): Filter Effects

- [x] Implement `GeodeFilterEngine` scaffolding with WebGPU compute pipeline.
  - [x] Gaussian blur (separable, two-pass compute) — first compute pipeline in Geode.
  - [x] feOffset (pixel shift via compute shader).
  - [x] feColorMatrix (4x5 matrix transform via compute shader — all type variants).
  - [x] feFlood (constant color fill via compute shader).
  - [x] feMerge (alpha-over composite of N inputs via sequential compute dispatches).
  - [x] feComposite (Porter-Duff compositing — all 7 operators including arithmetic).
  - [x] feBlend (W3C Compositing 1 — all 16 blend modes via compute shader).
  - [x] feMorphology (erode/dilate via 2D min/max kernel compute shader).
  - [x] feComponentTransfer (per-channel 256-entry LUT via storage buffer compute shader).
  - [x] feConvolveMatrix (NxM kernel convolution via compute shader, up to 5x5).
  - [ ] Color matrix (single-pass compute).
  - [ ] Turbulence (Perlin noise compute shader).
  - [ ] Displacement map.
  - [ ] Lighting (diffuse and specular, point/distant/spot light sources).
- [x] Implement `pushFilterLayer`/`popFilterLayer` on `RendererGeode`.
- [x] Implement filter graph execution: route intermediate textures between compute passes
  matching the `FilterGraph` node topology (scaffolding — unsupported primitives pass through).
- [x] Run full resvg filter test suite — all filter categories now run on Geode with the
  `FilterEffects` feature flag enabled.

## Testing and Validation

**Golden image tests:** Geode shares the existing golden image infrastructure. Backend-specific
thresholds accommodate GPU vs. CPU antialiasing differences, following the same conventions as
tiny-skia-cpp (per-pixel threshold first, `maxMismatchedPixels` as last resort).

**Structural tests:** Once `MockRendererInterface` (Phase 4 of renderer interface design) ships,
structural assertions apply identically since `RendererDriver` is backend-agnostic.

**GPU-specific tests:**
- Resource leak detection: track `wgpu::Buffer`/`wgpu::Texture` create/destroy counts per frame.
- Cache hit/miss counters: verify ECS caching is effective across re-renders.
- Timestamp profiling: regression detection for per-frame GPU time.

**Resvg test suite:** Target is matching the archived full-Skia renderer's pass rate. Acceptable parity gaps are
limited to sub-pixel antialiasing differences (GPU vs. CPU rasterizer precision).

**Text rendering:** Test across all three configurations (base, text-full, geode) per the
existing testing conventions.

## Dependencies

| Dependency | Version | Purpose | License |
|------------|---------|---------|---------|
| Dawn | HEAD (pinned) | WebGPU implementation | BSD-3-Clause |
| wgpu-native | (alternative) | Lighter-weight WebGPU if Dawn is too heavy | Apache-2.0/MIT |

Dawn is the primary choice for its maturity and Chromium backing. If binary size is a concern
for embedded use cases, `wgpu-native` (the Rust WebGPU implementation with a C API) is an
alternative worth evaluating.

### Slug Algorithm: Licensing and Implementation Strategy

**Geode does NOT use the Slug reference implementation code.** Donner is ISC-licensed, and the
reference shaders on GitHub are MIT-licensed. While MIT is permissive, mixing MIT attribution
requirements into an ISC project adds unnecessary license complexity. More importantly, a
clean-room implementation lets us:

- Design data structures around donner types (`Path`, `Transform2d`) from the start
- Use WGSL (WebGPU Shading Language) natively instead of porting HLSL/GLSL
- Optimize for our specific use case (SVG paths, not just text glyphs)
- Avoid inheriting any design decisions that don't fit our architecture

**What we use from Slug:**

| Resource | How we use it | License status |
|----------|--------------|----------------|
| Patent US10373352B1 | Implements the patented algorithm | Public domain (dedicated 2026-03-17) |
| JCGT 2017 paper | Mathematical reference for winding number, root eligibility | Academic publication (ideas not copyrightable) |
| "A Decade of Slug" blog post | Design lessons (dilation > supersampling, no band-split, no emoji loops) | Ideas not copyrightable |
| Reference shader code (GitHub) | **Not used** — clean-room WGSL implementation | MIT (we avoid to keep ISC clean) |

**Implementation approach:**

1. Implement the winding number and root eligibility math from the JCGT paper specification.
2. Implement dynamic dilation from the mathematical derivation in the blog post.
3. Write all shaders in WGSL from scratch, structured around our `GeodePathEncoder` output
   format and `GeoPaint` variant types.
4. Validate correctness against golden images (same infrastructure as other backends), not
   against reference shader output.

## Alternatives Considered

**Tessellation-based GPU rendering (pathfinder, piet-gpu, Vello):**
These approaches convert paths to triangle meshes on the CPU or via compute shaders. They
achieve good GPU utilization but require complex tessellation logic, LOD management, and produce
quality artifacts at extreme zoom levels. Slug's analytical per-pixel evaluation avoids these
issues entirely and produces consistent quality at any scale. However, Vello's compute-shader
approach is worth monitoring as a future alternative if Slug's per-pixel evaluation proves too
expensive for extremely complex paths.

**MSDF (Multi-channel Signed Distance Fields):**
Good for text at moderate scales but fundamentally limited for arbitrary SVG paths. Requires
offline atlas generation, struggles with sharp corners and thin strokes, and needs atlas
regeneration when the glyph set changes. Not viable as a general SVG renderer.

**Skia Graphite (next-gen Skia GPU backend):**
Would provide excellent rendering quality but brings the full Skia dependency (~50 MB), is not
designed for embedding in third-party GPU contexts, and does not give us control over the
rendering pipeline for SVG-specific optimizations.

**Native Vulkan/Metal directly (no WebGPU abstraction):**
Maximum performance but requires maintaining two completely separate codepaths. WebGPU provides
>95% of native performance with a single codebase. The future path section below addresses how
to drop to native APIs if needed.

## Open Questions

1. **Stroke rendering strategy:** v1 uses `Path::strokeToFill()` (CPU-side stroke expansion)
   then renders the expanded path via Slug. GPU-side stroke expansion is a future optimization
   for animated stroke widths — worth investigating post-v1 if stroke-heavy SVGs show CPU
   bottlenecks in `strokeToFill()`.

2. **Dawn vs. wgpu-native:** Dawn is more mature but significantly larger. For the embeddability
   goal, binary size matters. Need to evaluate both and choose based on real measurements.

3. **Offscreen render target pooling:** Compositing-heavy SVGs (many opacity layers, masks,
   patterns) can require many offscreen textures. Should we implement a render target pool with
   LRU eviction, or rely on the WebGPU allocator?

4. **Subpixel text rendering:** Slug produces grayscale antialiasing. Subpixel (LCD) rendering
   requires 3x horizontal resolution and OS-specific gamma tables. Is this in scope, or is
   grayscale AA sufficient for SVG use cases?

5. **Maximum path complexity:** Slug's fragment shader cost scales with curves-per-band. For
   extremely complex paths (thousands of curves), should we implement a fallback (e.g., CPU
   rasterize to texture) or rely on adaptive band counts to keep per-pixel cost bounded?

6. **WGSL numerical precision:** The root eligibility method requires careful floating-point
   handling to maintain Slug's robustness guarantee. WGSL's `f32` precision may differ from
   HLSL/GLSL in edge cases. Need to validate winding number correctness across GPU vendors
   (particularly for near-tangent curve intersections and cusps) early in Phase 1.

## Future Work

- **Native Vulkan/Metal backends:** Implement `GeodeDevice` variants that target Vulkan/Metal
  directly via MoltenVK, bypassing the WebGPU abstraction for maximum performance in
  platform-specific deployments.
- **Render pass injection:** Allow host applications to provide an active render pass encoder
  so Geode can append draw calls without an intermediate render target.
- **Animation optimization:** Leverage ECS caching for SMIL/CSS animation — only re-encode
  paths that actually changed between frames, reuse all other GPU resources.
- **Composited rendering integration:** Connect with the composited rendering design for
  layer-based caching, enabling sub-frame updates when only part of the SVG tree is dirty.
- **Compute shader path encoding:** Move band decomposition from CPU to a compute shader for
  fully GPU-resident path updates.
- **HDR and wide-gamut color:** WebGPU supports `rgba16float` textures — extend the pipeline
  for Display P3 / Rec. 2020 color spaces.

## Long-Term Vision: Geode as a Standalone Rendering Engine

> **Timeline:** Everything in this section is post-v1. Phases 1–7 above deliver a complete 2D
> SVG rendering backend and standalone 2D graphics library. The 3D extensions below are a
> separate initiative that builds on the v1 foundation.

The initial phases treat Geode as a Donner rendering backend and general-purpose 2D library.
The long-term vision is broader: evolve Geode into a standalone, embeddable rendering engine
suitable for game engines, creative tools, and interactive applications, extending beyond 2D
into 3D.

### Motivation

Donner's ECS architecture, incremental invalidation, and Geode's GPU-native pipeline form a
foundation that generalizes well beyond SVG. A game engine or UI toolkit embedding Donner for
SVG rendering already has a high-performance GPU pipeline and an ECS registry — extending that
to 3D content is a natural progression rather than a separate system.

The Slug algorithm itself is dimension-agnostic for curve evaluation. Its vertex shader already
handles perspective projection and dynamic dilation in 3D clip space. Extending to 3D meshes
and scenes requires adding depth management, lighting, and a camera/projection system — not
replacing the core rendering approach.

### Standalone API Surface

The 2D `GeoEncoder`/`GeoSurface` API is designed to be standalone from day one — the
`donner::geode` namespace has no dependency on `donner::svg`. The standalone library extraction
(Phase 8) makes this usable without the SVG parser, CSS engine, or DOM.

For 3D, the API extends with new primitives:

```cpp
namespace donner::geode {

/// 3D transform (4x4 matrix).
using Transform3d = /* donner base type, TBD */;

/// Camera/projection configuration.
struct GeoCamera {
  Transform3d viewTransform;       // worldFromCamera inverse
  Transform3d projectionTransform; // perspective or orthographic

  static GeoCamera Perspective(double fovY, double aspect, double near, double far);
  static GeoCamera Orthographic(double left, double right, double bottom, double top,
                                double near, double far);
};

/// Extended encoder with 3D support.
class GeoEncoder3D : public GeoEncoder {
 public:
  // Camera and depth
  void setCamera(const GeoCamera& camera);
  void enableDepthTest(bool enable);

  // 3D mesh rendering
  void drawMesh(const GeoMesh& mesh, const Transform3d& transform, const GeoPaint& paint);

  // 3D vector paths — Slug curves rendered on a plane in 3D space.
  // The Slug vertex shader's perspective dilation handles AA correctly.
  void fillPath3D(const Path& path, const Transform3d& planeTransform,
                  FillRule fillRule, const GeoPaint& paint);

  // Text in 3D space (billboarded or world-anchored)
  void drawText3D(std::span<const GeoGlyph> glyphs, const Transform3d& transform,
                  const GeoPaint& paint, GeoTextAnchor anchor = GeoTextAnchor::World);

  // 3D lighting (for mesh rendering, not SVG filter feLighting)
  void setAmbientLight(const css::RGBA& color, double intensity);
  void addPointLight(const Vector3d& position, const css::RGBA& color, double intensity,
                     double range);
  void addDirectionalLight(const Vector3d& direction, const css::RGBA& color,
                           double intensity);
  void clearLights();
};

/// Triangle mesh with optional normals, UVs, and vertex colors.
struct GeoMesh {
  std::span<const Vector3d> positions;
  std::span<const Vector3d> normals;          // Optional, per-vertex
  std::span<const Vector2d> uvs;              // Optional, per-vertex
  std::span<const css::RGBA> vertexColors;    // Optional, per-vertex
  std::span<const uint32_t> indices;          // Triangle list

  /// Upload to GPU and return a cached handle.
  GeoMeshHandle upload(GeodeDevice& device) const;
};

}  // namespace donner::geode
```

### 3D Extension Architecture

```
donner::geode (standalone library)
├── GeoEncoder        (2D — inherited from Donner backend work)
├── GeoEncoder3D      (3D — extends GeoEncoder)
├── GeodeDevice       (shared — WebGPU device/queue/allocator)
├── GeodePipeline     (shared — Slug shaders, mesh shaders, lighting)
├── GeodeScene        (new — ECS-backed scene graph for 3D)
│   ├── TransformComponent3D
│   ├── MeshComponent
│   ├── LightComponent
│   ├── CameraComponent
│   └── VectorOverlayComponent  (2D SVG content placed in 3D)
└── GeodePathEncoder  (shared — Slug band encoding, works in 2D and 3D)
```

**Key design decisions for 3D:**

- **Depth buffer management:** 2D SVG rendering uses painter's algorithm (back-to-front via
  `drawOrder`). 3D adds a depth buffer for opaque geometry, with a separate translucent pass
  using order-independent transparency (OIT) or sorted back-to-front for blended content.
- **Slug in 3D:** Vector paths rendered in 3D space continue to use Slug's analytical curve
  evaluation. The vertex shader's existing perspective dilation produces correct AA even for
  paths viewed at steep angles. This is a key advantage — text and vector art remain crisp
  regardless of camera angle, unlike texture-based approaches.
- **Mixed 2D/3D:** A `VectorOverlayComponent` allows SVG content (rendered via the existing
  Donner pipeline) to be placed on a plane in 3D space. This enables use cases like in-world
  UI panels, labels, and HUD elements rendered as resolution-independent vectors.
- **ECS continuity:** The 3D scene uses the same entt registry and component patterns as Donner.
  GPU resource caching (`GeodePathCacheComponent`, etc.) works identically for 2D and 3D
  content. The incremental invalidation system extends to 3D transform hierarchies.

### Standalone Packaging

When used standalone (outside Donner), Geode would be packaged as:

```
libgeode
├── geode/           (public headers)
│   ├── GeoEncoder.h
│   ├── GeoEncoder3D.h
│   ├── GeoSurface.h
│   ├── GeoPaint.h
│   ├── GeoMesh.h
│   ├── GeoCamera.h
│   ├── GeodeDevice.h
│   └── GeodeScene.h
├── geode/base/      (core types extracted from donner/base/)
│   ├── Vector2.h, Vector3.h
│   ├── Transform.h, Transform3d.h
│   ├── Box.h
│   └── Color.h
└── geode/ecs/       (thin entt wrapper)
    └── Registry.h
```

Donner base types (`Vector2d`, `Transform2d`, `Box2d`, `Path`, etc.) would be extracted
into a shared `geode/base/` layer that both Donner and standalone Geode depend on. This avoids
type duplication while allowing Geode to be used without pulling in the SVG parser, CSS engine,
or DOM layer.

### Phased Roadmap (Post-v1)

These phases follow after the core Geode v1 phases (1–7) described above.

#### Phase 8: Standalone Library Extraction

- [ ] Verify `//geode` Bazel package has no transitive dependency on `//donner/svg` — the 2D
  API is designed standalone from the start; this phase is about validation and packaging.
- [ ] Extract shared base types (`Vector2`, `Transform2`, `Box2`, `Path`, color types)
  into `//geode/base` or `//donner/base` as a shared foundation.
- [ ] Publish standalone API documentation and usage guide.
- [ ] Provide examples:
  - [ ] SDL/GLFW window with Geode drawing 2D vector content (paths, gradients, text).
  - [ ] Simple UI toolkit demo (buttons, panels, text) using `GeoEncoder` directly.
  - [ ] Data visualization (charts, graphs) using `GeoEncoder` directly.

#### Phase 9: 3D Foundation

- [ ] Add `Vector3`, `Transform3d` (4x4 matrix), `GeoCamera` types.
- [ ] Implement depth buffer management on `GeodePipeline`.
- [ ] Implement `GeoEncoder3D::setCamera` and perspective/orthographic projection.
- [ ] Implement `fillPath3D` — Slug paths on a plane in 3D space with perspective-correct AA.
- [ ] Implement `drawText3D` — glyph instances with 3D transforms.
- [ ] Provide example: spinning vector text and SVG paths in 3D.

#### Phase 10: Mesh Rendering

- [ ] Implement `GeoMesh` upload and GPU buffer management.
- [ ] Implement `drawMesh` with solid and gradient paints.
- [ ] Implement per-vertex normals and basic PBR-style lighting pipeline.
  - [ ] Ambient, point, and directional lights.
  - [ ] Diffuse + specular BRDF in fragment shader.
- [ ] Implement textured meshes (UV-mapped `wgpu::Texture`).
- [ ] Implement `GeodeScene` ECS-backed scene graph with transform hierarchy.

#### Phase 11: Mixed 2D/3D and Advanced Features

- [ ] Implement `VectorOverlayComponent`: SVG content rendered to texture, placed on 3D quads.
- [ ] Implement order-independent transparency for mixed opaque/translucent scenes.
- [ ] Shadow mapping for directional and point lights.
- [ ] Implement instanced mesh rendering for repeated geometry.
- [ ] Frustum culling using ECS bounding-box components.
- [ ] Provide example: game-engine-style scene with 3D meshes, in-world SVG UI panels, and
  Slug-rendered text labels.
