# tiny-skia-cpp

A fast, minimal CPU-only 2D rendering library for C++20.

Faithfully ported from [tiny-skia](https://github.com/linebender/tiny-skia), which
implements a subset of [Skia](https://skia.org/)'s rendering algorithms in Rust.
**tiny-skia-cpp** brings the same proven rasterization and blending algorithms to C++
with native SIMD acceleration and zero external dependencies.

## Highlights

- **Pixel-accurate** — bit-exact output matching Rust tiny-skia, enforced by golden-image tests and `-ffp-contract=off`
- **Fast** — up to 1.9× faster than the Rust implementation with SIMD
- **SIMD accelerated** — native backends for x86-64 (AVX2+FMA) and ARM64 (NEON), plus a portable scalar fallback
- **Tiny & fast to build** — minimal codebase, zero external dependencies, fast compile times
- **Embeddable** — single `add_subdirectory()` or Bazel dep; two static libraries, zero runtime dependencies
- **Skia algorithms** — same rasterization, scanline conversion, and pixel blending algorithms as Google's Skia

## Performance

Rendering 512×512 px workloads, normalized to Rust tiny-skia = 1.0×.
ARM measured on Apple Silicon (NEON); x86 on Intel/AMD (AVX2+FMA).

```text
                                                          higher is better
FillRect (512×512)
  C++ SIMD (ARM)    ██████████████████████████████  1.9×
  C++ SIMD (x86)    █████████████████████·········  1.3×
  Rust (tiny-skia)  ████████████████··············  1.0×

FillPath (512×512)
  C++ SIMD (ARM)    ██████████████████████········  1.4×
  C++ SIMD (x86)    ███████████████████···········  1.2×
  Rust (tiny-skia)  ████████████████··············  1.0×
```

SIMD speedup over C++ Scalar: up to 2.3× (x86 AVX2) / 1.9× (ARM NEON).

## Quick Start

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

## Requirements

- C++20 compiler (GCC 11+, Clang 14+)
- [Bazel](https://bazel.build/) 7+ (or [Bazelisk](https://github.com/bazelbuild/bazelisk)), **or** [CMake](https://cmake.org/) 3.16+

## Build (Bazel)

```bash
# Install Bazelisk (if bazel is not already installed)
./tools/env-setup.sh

# Build everything
bazel build //...

# Run all tests
bazel test //...
```

The `env-setup.sh` script accepts optional environment variables:

- `INSTALL_DIR` — where to place the `bazel` binary (default: `/usr/local/bin`)
- `BAZELISK_VERSION` — Bazelisk release tag (default: `v1.25.0`)

## Build (CMake)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces two static library targets:

- **`tiny_skia`** — native SIMD (AVX2+FMA on x86_64, NEON on ARM64)
- **`tiny_skia_scalar`** — portable scalar-only backend

To embed in your own CMake project:

```cmake
add_subdirectory(path/to/tiny-skia-cpp)
target_link_libraries(your_target PRIVATE tiny_skia)
```

## Examples

Each example produces a PNG in the current working directory:

```bash
bazel run //examples:fill              # Two overlapping cubic Bézier fills
bazel run //examples:stroke            # Dashed star polyline with round caps
bazel run //examples:hairline          # Thin strokes at decreasing widths
bazel run //examples:linear_gradient   # Linear gradient fill on a cubic path
bazel run //examples:mask              # Masked donut ring via even-odd clip
bazel run //examples:pattern           # Repeating triangle pattern in a circle
bazel run //examples:gamma             # Color-space comparison (Linear/sRGB)
bazel run //examples:image_on_image    # Compositing one pixmap onto another
bazel run //examples:large_image       # 20000×20000 stress test with masking
```

## Architecture

The rendering pipeline flows through five major subsystems:

```
PathBuilder → Path → EdgeBuilder → Scan → Pipeline → Pixmap
                                     ↑         ↑
                                   path64    shaders
                                              wide
```

1. **Core** (`src/tiny_skia/`) — fundamental types (Pixmap, Color, Path, Transform,
   Mask, Geom) and algorithms (edge building, clipping, fixed-point math).
2. **Scan** (`src/tiny_skia/scan/`) — scanline rasterization for fills and hairlines.
3. **Pipeline** (`src/tiny_skia/pipeline/`) — rendering stage execution (blend,
   composite, sample, store) with high-precision and low-precision fast paths.
4. **Shaders** (`src/tiny_skia/shaders/`) — solid colors, linear/radial/sweep
   gradients, and pixmap patterns.
5. **Wide** (`src/tiny_skia/wide/`) — SIMD vector wrappers (F32x4T, F32x8T, etc.)
   for data-parallel execution.
6. **Path64** (`src/tiny_skia/path64/`) — 64-bit path math for precision-sensitive
   subdivision operations.

The drawing API lives on `Pixmap` and `MutablePixmapView`:
`fillRect`, `fillPath`, `strokePath`, `drawPixmap`, and `applyMask`.

### SIMD Backends

The `wide/` layer provides three compile-time backends with an identical API:

| Backend | Platforms | Define |
|---------|-----------|--------|
| **x86 AVX2+FMA** | x86_64 (Intel/AMD) | `TINYSKIA_CFG_IF_SIMD_NATIVE` |
| **ARM64 NEON** | Apple Silicon, AArch64 | `TINYSKIA_CFG_IF_SIMD_NATIVE` |
| **Scalar** | All platforms (portable fallback) | `TINYSKIA_CFG_IF_SIMD_SCALAR` |

Backend selection happens at compile time via `wide/backend/BackendConfig.h`.
All backends produce bit-exact results.

## Module Map

| Directory | Responsibility | Key Types / Files |
|-----------|---------------|-------------------|
| `src/tiny_skia/` | Core types and algorithms | Pixmap, Color, Path, Transform, Mask, Geom, Edge, Blitter |
| `src/tiny_skia/scan/` | Scanline rasterization | Hairline, HairlineAa, Path, PathAa |
| `src/tiny_skia/pipeline/` | Rendering pipeline stages | RasterPipelineBuilder, Highp, Lowp, Blitter |
| `src/tiny_skia/shaders/` | Fill shaders and gradients | LinearGradient, RadialGradient, SweepGradient, Pattern |
| `src/tiny_skia/wide/` | SIMD vector wrappers | F32x4T, F32x8T, I32x4T, U32x4T, U32x8T, U16x16T |
| `src/tiny_skia/wide/backend/` | Platform-specific SIMD | Scalar*, X86Avx2Fma*, Aarch64Neon* |
| `src/tiny_skia/path64/` | 64-bit path math | Point64, Quad64, Cubic64, LineCubicIntersections |
| `examples/` | Runnable C++ examples (PNG output) | fill, stroke, gradient, mask, pattern, ... |
| `tests/integration/` | Golden-image integration tests | FillTest, StrokeTest, GradientsTest, ... |
| `tests/benchmarks/` | Performance benchmarks | RenderPerfBench |
| `tests/rust_ffi/` | Rust FFI for cross-validation | tiny_skia_ffi |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development workflow, toolchain versions,
formatting, and troubleshooting guidance.

## License

BSD-3-Clause — see [LICENSE](LICENSE) for details.
