# Design: Geode — GPU-Native Rendering Backend

**Status:** Planned
**Author:** Jeff McGlynn
**Created:** 2026-04-07

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
| RendererSkia | CPU/GPU (Skia) | ~50 MB | Full feature parity, reference quality | Heavy dependency, not embeddable |
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
Using [Dawn](https://dawn.googlesource.com/dawn) (Google's WebGPU implementation) as the
backend gives us:

- Cross-platform support (Windows, macOS, Linux, Android, iOS, Web via wasm)
- Modern GPU features (compute shaders for v2 filters, storage buffers)
- No platform-specific code in the renderer
- Future path to native Vulkan/Metal for applications that need it

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
        RendererSkia  TinySkia  RendererGeode|
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
shader simplicity (fewer branches, no supersampling, no bidirectional rays) consistently
outperforms more complex variants due to reduced divergence:

```
For each curve in this band's curve list:
    1. Compute ray-curve intersection roots
    2. Apply root eligibility test (filters out tangent touches, endpoints
       already counted by adjacent curves, and numerical noise)
    3. Accumulate winding number contribution (+1 or -1 per valid crossing)

Apply fill rule to winding number → binary inside/outside
Compute AA coverage from sub-pixel edge distance
Multiply coverage × paint color (solid, gradient, or pattern sample)
```

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
  - [ ] Implement `cubicToQuadratic()`.
  - [ ] Implement `toMonotonic()`.
  - [ ] Implement `flatten()`.
  - [ ] Implement `strokeToFill()`.
- [x] Migrate `RendererInterface` from `PathSpline` to `Path`.
- [x] Migrate remaining callers, remove `PathSpline`.

### Phase 1: Foundation and Path Rendering

- [ ] Vendor Dawn (WebGPU) as a third-party dependency with Bazel build.
- [ ] Implement `GeodeDevice`: device/queue creation, surface management, buffer allocator.
- [ ] Implement `GeodePathEncoder`: `Path` → Slug band decomposition.
  - [ ] Call `path.cubicToQuadratic()` then `path.toMonotonic()` as preprocessing.
  - [ ] Band decomposition algorithm (adaptive band count).
  - [ ] Vertex buffer generation (bounding quads with dilation data + inverse Jacobian).
  - [ ] Curve data packing (quadratic control points, 2×16-bit band metadata).
- [ ] Implement Slug vertex shader: MVP transform, dynamic half-pixel dilation.
- [ ] Implement Slug fragment shader: ray-curve intersection, winding number, coverage.
  - [ ] Non-zero fill rule.
  - [ ] Even-odd fill rule.
- [ ] Implement `GeoEncoder` core: transform stack, solid color fill, path rendering.
- [ ] Implement `RendererGeode` skeleton: `beginFrame`/`endFrame`, `setTransform`,
  `drawPath` with solid fill.
- [ ] Add Bazel `--config=geode` backend selection.
- [ ] Run basic renderer tests (solid-fill SVGs) against golden images.

### Phase 2: Complete SVG Painting

- [ ] Implement stroke rendering via `Path::strokeToFill()` → Slug fill pipeline.
- [ ] Implement `GeodeGradientEncoder`: linear, radial, and sweep gradients.
  - [ ] Fragment shader gradient evaluation.
  - [ ] Spread modes: pad, reflect, repeat.
- [ ] Implement pattern tile rendering: render tile to offscreen texture, sample as repeating
  shader.
- [ ] Implement `drawRect` and `drawEllipse` optimized paths (skip band decomposition for
  axis-aligned primitives).
- [ ] Implement `drawImage`: textured quad with opacity and filtering.
- [ ] Implement `GeodeGradientCacheComponent` for ECS gradient caching.

### Phase 3: Compositing and Clipping

- [ ] Implement scissor-based clip rect.
- [ ] Implement stencil-based clip path (render path to stencil, draw with stencil test).
- [ ] Implement `pushIsolatedLayer`/`popIsolatedLayer`: offscreen render target allocation,
  opacity/blend-mode compositing.
  - [ ] Blend mode fragment shader (all 28 SVG/CSS blend modes).
- [ ] Implement mask compositing: render mask to offscreen, composite via alpha/luminance.
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
- [ ] Performance benchmarking: compare against RendererSkia and RendererTinySkia on the
  resvg test suite and complex real-world SVGs.
- [ ] Optimize: batch draw calls for paths sharing the same pipeline state.

### Phase 6: Embeddability

- [ ] Implement `GeodeEmbedConfig`: host-provided device/queue/format.
- [ ] Implement `setTargetTexture` for rendering into host-owned textures.
- [ ] Document embedding API and provide example integration code.
- [ ] Test embedded mode with a minimal SDL/GLFW host application.

### Phase 7 (v2): Filter Effects

- [ ] Implement `GeodeFilterEngine` using WebGPU compute shaders.
  - [ ] Gaussian blur (separable, two-pass compute).
  - [ ] Color matrix (single-pass compute).
  - [ ] Morphology (erode/dilate via compute).
  - [ ] Turbulence (Perlin noise compute shader).
  - [ ] Displacement map, component transfer, convolution matrix.
  - [ ] Lighting (diffuse and specular, point/distant/spot light sources).
  - [ ] Blend and composite operations.
- [ ] Implement `pushFilterLayer`/`popFilterLayer` on `RendererGeode`.
- [ ] Implement filter graph execution: route intermediate textures between compute passes
  matching the `FilterGraph` node topology.
- [ ] Run full resvg filter test suite.

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

**Resvg test suite:** Target is matching RendererSkia's pass rate. Acceptable parity gaps are
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
