# Design: SIMD vs Scalar vs Rust Benchmarking

**Status:** Implemented (with engine-core fairness update)
**Author:** Codex
**Created:** 2026-03-02

## Summary

- Add repeatable performance benchmarks to compare:
  - C++ build in native SIMD mode
  - C++ build in forced scalar mode
  - Rust tiny-skia via existing FFI bridge
- Use Google Benchmark with Bazel so results are produced from one consistent harness.

## Goals

- Add a Bazel benchmark target that compiles and runs on this repo's current toolchain.
- Measure identical rendering workloads for C++ and Rust in the same process.
- Provide a simple command path that shows SIMD/scalar/Rust relative performance.
- Keep benchmark code separate from correctness tests and rendering golden tests.

## Non-Goals

- No benchmark-driven production behavior changes in this batch.
- No attempt to replicate Rust's standalone Cargo bench suite exactly.
- No CI gating on absolute benchmark numbers (host noise makes fixed thresholds brittle).

## Next Steps

- Add release-build benchmark presets for less debug-build noise.
- Optionally wire benchmark runs into a non-blocking CI/perf dashboard workflow.

## Implementation Plan

- [x] Milestone 1: Benchmark infrastructure
  - [x] Step 1: Add `google_benchmark` dependency in `MODULE.bazel`.
  - [x] Step 2: Add `tests/benchmarks` Bazel package with native/scalar benchmark binaries.
- [x] Milestone 2: Comparable C++ and Rust workloads
  - [x] Step 1: Add benchmark source with matched C++ and Rust rendering workloads.
  - [x] Step 2: Expose backend mode metadata in benchmark output for traceability.
- [x] Milestone 3: Relative comparison workflow
  - [x] Step 1: Add a runner script that executes native + scalar benchmark binaries.
  - [x] Step 2: Parse results and print SIMD/scalar/Rust speedup summary.
- [x] Milestone 4: Engine-core fairness for Rust comparison path
  - [x] Step 1: Add prepared Rust FFI render-state handles (`paint`, `rect`, `transform`).
  - [x] Step 2: Update Rust benchmark loops to reuse prepared state and avoid per-iteration
    setup conversions.
  - [x] Step 3: Expose benchmark metadata indicating engine-core comparison mode.

## Proposed Architecture

- Add a new benchmark source under `tests/benchmarks/` that:
  - creates the same path/pixmap workload for C++ and Rust,
  - runs `tiny_skia::fillPath(...)` for C++ and `ts_ffi_fill_path(...)` for Rust,
  - publishes throughput counters (`pixels_per_second`) and iteration time.
  - uses prepared Rust FFI render state in the hot loop to keep Rust path comparable to
    C++ prebuilt paint/geometry setup.
- Build two C++ benchmark binaries:
  - `render_perf_bench_native` depends on `//src:tiny_skia_lib_native`
  - `render_perf_bench_scalar` depends on `//src:tiny_skia_lib_scalar`
- Keep Rust path fixed through `//tests/rust_ffi:tiny_skia_ffi`, so C++ mode changes are
  isolated while Rust baseline is measured in both runs.
- Add a small shell runner to invoke both binaries with stable benchmark flags and compute:
  - C++ SIMD speedup over C++ scalar
  - C++ SIMD speedup over Rust
  - C++ scalar speedup over Rust
  - build mode defaults to `opt` for benchmark timing runs.

## Security / Privacy

- No new trust boundaries. Benchmarks use local code and deterministic synthetic scenes.
- No external user data ingestion.

## Testing and Validation

- Build validation:
  - `bazel build //...`
- Test validation (no behavior regressions):
  - `bazel test //...`
- Benchmark smoke validation:
  - run benchmark binaries in native and scalar modes and confirm summary output includes all
    expected comparisons.

## Recent Update: 2026-03-02 (Engine-Core Fairness)

- Added prepared-state Rust FFI APIs in `tests/rust_ffi`:
  - `ts_ffi_paint_new_solid_rgba8` / `ts_ffi_paint_free`
  - `ts_ffi_rect_from_ltrb` / `ts_ffi_rect_free`
  - `ts_ffi_transform_from_row` / `ts_ffi_transform_free`
  - `ts_ffi_fill_path_prepared`
  - `ts_ffi_fill_rect_prepared`
- Updated benchmark loops to use prepared Rust state objects outside the measurement loop.
- Added benchmark custom context:
  - `comparison_mode=engine_core`
  - `rust_ffi_mode=prepared_state`
