# tiny-skia-cpp {#mainpage}

A fast, minimal CPU-only 2D rendering library for C++20 — a faithful port of
[tiny-skia](https://github.com/linebender/tiny-skia), which implements a subset of
[Skia](https://skia.org/)'s rendering algorithms in Rust.

**tiny-skia-cpp** brings the same proven rasterization and blending algorithms to C++
with native SIMD acceleration and zero external dependencies.

- **High fidelity** — near-pixel-accurate output validated against Rust tiny-skia with tolerance-based golden-image tests
- **Fast** — up to 1.9× faster than the Rust implementation with SIMD
- **SIMD accelerated** — native backends for x86-64 (AVX2+FMA) and ARM64 (NEON)
- **Tiny & fast to build** — minimal codebase, zero external dependencies
- **Embeddable** — two static libraries, zero runtime dependencies
- **Skia algorithms** — same rasterization, scanline, and blending algorithms as Google's Skia

## Performance

Speedup vs Rust tiny-skia on 512×512 px workloads (higher is better):

| Workload | C++ SIMD (ARM) | C++ SIMD (x86) | Rust (tiny-skia) |
|----------|----------------|----------------|------------------|
| FillRect | **1.9×** | **1.3×** | 1.0× |
| FillPath | **1.4×** | **1.2×** | 1.0× |

SIMD speedup over C++ Scalar: up to 2.3× (x86 AVX2) / 1.9× (ARM NEON).

## Quick start

```cpp
#include "tiny_skia/Canvas.h"
#include "tiny_skia/PathBuilder.h"
#include "tiny_skia/Pixmap.h"

using namespace tiny_skia;

// Create a 500×500 RGBA pixmap (transparent black).
auto pixmap = Pixmap::fromSize(500, 500).value();
Canvas canvas(pixmap);

// Build a triangle path.
PathBuilder pb;
pb.moveTo(250, 50);
pb.lineTo(450, 400);
pb.lineTo(50, 400);
pb.close();
auto path = pb.finish().value();

// Fill with a semi-transparent green.
Paint paint;
paint.setColorRgba8(0, 200, 80, 180);
canvas.fillPath(path, paint, FillRule::Winding);

// pixmap now contains the rendered triangle.
// Call pixmap.releaseDemultiplied() to get straight-alpha RGBA bytes for PNG encoding.
```

## Key types

| Type | Purpose |
|------|---------|
| @ref tiny_skia::Canvas | Drawing surface backed by a Pixmap or MutablePixmapView (fillRect, fillPath, strokePath, drawPixmap, applyMask) |
| @ref tiny_skia::Pixmap | Owned RGBA pixel buffer |
| @ref tiny_skia::MutablePixmapView | Non-owning mutable view into a pixmap |
| @ref tiny_skia::Path | Immutable vector path (lines, quads, cubics) |
| @ref tiny_skia::PathBuilder | Builder for constructing Path objects |
| @ref tiny_skia::Paint | Shader + blend mode + anti-alias settings |
| @ref tiny_skia::Transform | 2D affine transformation matrix |
| @ref tiny_skia::Mask | 8-bit alpha mask for clipping |
| @ref tiny_skia::Stroke | Stroke width, line cap, line join, dash pattern |
| @ref tiny_skia::Color | Floating-point RGBA color \[0,1\] |
| @ref tiny_skia::ColorU8 | 8-bit RGBA color |

## Shader types

The @ref tiny_skia::Shader variant holds one of:
- @ref tiny_skia::Color — solid color
- @ref tiny_skia::LinearGradient — two-point linear gradient
- @ref tiny_skia::RadialGradient — two-point conical gradient
- @ref tiny_skia::SweepGradient — angular sweep gradient
- @ref tiny_skia::Pattern — pixmap-based pattern (tiled/clamped)

## Examples

Full examples are in the `examples/` directory:
- `fill.cpp` — basic path filling
- `stroke.cpp` — dashed stroke with round caps
- `linear_gradient.cpp` — linear gradient shader
- `mask.cpp` — alpha mask clipping
- `pattern.cpp` — pixmap pattern shader
