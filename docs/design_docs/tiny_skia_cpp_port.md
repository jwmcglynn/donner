# Design: Tiny-skia C++ Backend Port

## Summary
- Port the vendored Rust tiny-skia rasterizer to modern C++20 so Donner has a native backend without
  Rust toolchain dependencies.
- Deliver a self-contained backend in `donner/backends/tiny_skia_cpp` that plugs into the existing
  ECS rendering pipeline once the renderer abstraction is ready.
- Preserve correctness parity with the Rust implementation while keeping the surface maintainable
  and extensible for SIMD paths.

## Goals
- Match tiny-skia rendering behavior with a readable, testable C++20 implementation.
- Provide a canvas-style API and surface management compatible with Donner utilities.
- Keep clear ownership rules, minimal unsafe tricks, and documented SIMD/CPU extension seams.

## Non-Goals
- Rewriting Donner's higher-level SVG pipeline or public SVG APIs.
- Supporting every upstream tiny-skia build target (e.g., `no_std`, all CPU features) immediately.
- Shipping Rust FFI bindings; this effort is pure C++.

## Next Steps
- Land Phase 1 scaffolding: Bazel targets, core value types, PNG IO via existing utilities, and an
  `Expected<T, Error>` helper with unit tests.
- Outline initial `_tests.cc` coverage for scaffolding modules to lock API shapes.

## Implementation Plan
- [ ] Phase 1: add `donner/backends/tiny_skia_cpp/BUILD.bazel`, scaffold `Color`, `Transform`,
      `Pixmap`, `Canvas`, PNG IO, and `Expected<T, Error>` with tests.
- [ ] Phase 2: port path data structures and stroking/dashing math with geometry `_tests.cc`.
- [ ] Phase 3: add gradients, patterns, blend modes, and pipeline stages plus golden-image tests.
- [ ] Phase 4: expand `Canvas` draw APIs and canvas-level tests that render PNGs for comparison.
- [ ] Phase 5: add optional SSE2/AVX2/NEON implementations behind Bazel `select()` copts with perf
      checks.
- [ ] Phase 6: document divergences from Rust and plan the future renderer-integration seam.

## Background
Tiny-skia is a compact 2D rasterizer that mirrors a subset of Skia. We vendor the Rust crate in
`third_party/tiny-skia`. A C++ port removes Rust toolchain dependencies, shares code across C++
first platforms, and lets us tune performance with Donner utilities and compiler flags.

## Requirements and Constraints
- C++20, Google style, and repo conventions (`.roo/rules/01-coding-style.md`); namespace
  `donner::backends::tiny_skia_cpp`.
- Keep lines under 100 characters and honor `clang-format`; headers start with `#pragma once` and
  `/// @file`.
- Prefer `std::string_view`/`std::span`, existing math/string helpers, and avoid Abseil; rely on
  C++20 STL plus Donner primitives.
- Guard against UB-prone aliasing; assert preconditions with `UTILS_RELEASE_ASSERT` or debug
  assertions.
- Testing: gTest/gMock; add fuzzers for parser-like paths (PNG, path parsing) when feasible.
- Dependencies: no new third-party libraries without justification; reuse Donner PNG utilities.

## Proposed Architecture
- **Layout:** Flat Bazel-native layout under `donner/backends/tiny_skia_cpp` with headers beside
  sources (`Pixmap.h/.cc`, `Path.h/.cc`, `Stroke.h/.cc`, `Shader*.h/.cc`, `Pipeline*.h/.cc`,
  `BlendMode.h/.cc`, `Mask.h/.cc`, `ImageIO.h/.cc`). Tests sit next to implementations with
  `_tests.cc`; shared helpers live in small `*_test_utils.h/.cc` files. `data/` holds fixtures and
  goldens mirrored from upstream tests.
- **API surface:** Canvas-style API (`Canvas`, `Paint`, `Stroke`, `Path`, `Shader`, `Pixmap`)
  modeled on tiny-skia semantics. Shaders and blend modes exposed via enums with `operator<<` for
  debugging.
- **Integration seam:** No immediate wiring to `RenderingInstanceComponent`; keep seams ready for a
  future `AbstractRenderer` adapter. Use `RcString` or `std::span<uint8_t>` for surface identifiers
  and pixel views where it reduces copying.
- **SIMD strategy:** Keep scalar implementations as the default. Add specialized files (e.g.,
  `PipelineSimdAvx2.cc`) gated by Bazel copts/selects with clear fallbacks.

## Porting Strategy
- Semantic port rather than line-by-line; mirror algorithmic behavior and lock correctness with
  unit/golden tests.
- Map Rust crates (`tiny-skia`, `tiny-skia-path`) to C++ modules for pixmap, path, stroke, shaders,
  and pipeline. Shared math helpers (`NormalizedF32`, `NoStdFloat`) become lightweight structs with
  range checks.
- Replace Rust `Option`/`Result` with `std::optional` and a thin `Expected<T, Error>` aligned to
  `std::expected`; keep heap usage minimal via stack storage and small arenas.
- Gate accelerated scanline and gradient code behind feature macros and Bazel copts; add intrinsics
  only after scalar parity is proven.

## Build and Tooling Plan
- Bazel `cc_library`/`cc_test` targets under `donner/backends/tiny_skia_cpp/BUILD.bazel` with
  `select()`-guarded copts for SIMD variants; keep tests colocated with sources. Disable Rust
  dependencies in these targets.
- Mirror targets in the experimental CMake flow where practical.
- Use `clang-format` and `buildifier` for source and BUILD files; keep upstream attribution files in
  the backend directory.

## Security / Privacy
- Untrusted inputs are limited to PNG and any path data consumed by the canvas API. Validate bounds,
  sizes, and decode outcomes; reject malformed data via `Expected<T, Error>` or assertions.
- Add fuzzing/negative tests around PNG decode/encode and path parsing equivalents to catch parser
  edge cases. Avoid logging pixel data or untrusted strings without redaction.

## Testing and Validation
- Unit tests: port stroke geometry, gradients, clipping, and blend mode tests from the Rust suite
  into gTest with deterministic fixtures.
- Golden images: render a curated subset of `third_party/tiny-skia/tests/images` and diff PNG
  outputs with tolerance-based comparisons.
- Integration: run selected `donner/svg/renderer` tests with the C++ backend enabled to ensure
  pipeline compatibility.
- Fuzzing: consider libFuzzer harnesses for PNG IO and path parsing; evaluate coverage to ensure
  critical parsers are exercised.
- Performance: microbench gradients, stroking, and fill pipelines; compare scalar vs SIMD on x86-64
  and ARM64 hardware.

## Dependencies
- No new external libraries planned; rely on existing Donner utilities and the vendored tiny-skia
  assets for test fixtures.

## Risks and Mitigations
- Behavior drift: mirror upstream tests and golden comparisons to lock correctness.
- SIMD divergence: keep scalar as truth; gate SIMD with feature flags and CI matrix coverage.
- Complexity creep: align module boundaries with Donner architecture and reuse shared utilities.
- Maintenance cost: document divergences and keep a small compatibility layer to pull upstream
  fixes.

## Future Work
- [ ] Integrate with the forthcoming renderer abstraction once available.
- [ ] Expand SIMD coverage beyond initial hotspots if perf warrants it.
