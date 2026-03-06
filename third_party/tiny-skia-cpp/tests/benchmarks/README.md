# SIMD vs Scalar vs Rust Benchmarks

This package uses Google Benchmark to compare rendering throughput for:

- C++ in native SIMD mode (`//src:tiny_skia_lib_native`)
- C++ in forced scalar mode (`//src:tiny_skia_lib_scalar`)
- Rust tiny-skia via FFI (`//tests/rust_ffi:tiny_skia_ffi`)

Benchmark intent: engine-core vs engine-core.
Rust benchmark paths use prepared FFI state (`paint`, `rect`, `transform`) built once outside
the benchmark loop so iteration time reflects rendering work, not per-iteration setup.

## Run individual binaries

```bash
bazel run -c opt //tests/benchmarks:render_perf_bench_native
bazel run -c opt //tests/benchmarks:render_perf_bench_scalar
```

## Run relative comparison summary

```bash
./tests/benchmarks/run_render_perf_compare.sh
```

This executes both benchmark binaries, writes CSV artifacts under `.benchmarks/`,
and prints speedup ratios for `FillPath(512x512)` and `FillRect(512x512)`.
The script runs `bazel run -c opt` by default.

## Run regression guard test

```bash
bazel test -c opt //tests/benchmarks:render_perf_regression_test
```

This test uses Google Benchmark binaries as the measurement source and enforces
architecture-specific low/high watermarks for the same derived ratios that
`run_render_perf_compare.sh` prints:

- `simd_over_scalar` (`scalar_cpp_ns / native_cpp_ns`)
- `simd_over_rust` (`rust_avg_ns / native_cpp_ns`)

The guard validates both metrics for `FillPath(512x512)` and `FillRect(512x512)`,
with architecture-specific watermarks (`arm64`, `x86`) set to a tight ±25% band
around calibrated baselines.
