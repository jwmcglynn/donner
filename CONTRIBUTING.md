# Contributing to tiny-skia-cpp

## Toolchain Requirements

| Tool | Version | Notes |
|------|---------|-------|
| C++ compiler | GCC 11+, Clang 14+, or MSVC 19.30+ | Must support C++20 |
| Bazel | 7+ (via Bazelisk) | Primary build system |
| CMake | 3.16+ | Secondary build system |
| clang-format | Any recent version | Google style, 100-column limit |
| buildifier | Any recent version | Bazel file formatting |

Run `./tools/env-setup.sh` to install Bazelisk if you don't have Bazel.

## Development Workflow

1. **Design first** — for non-trivial changes, write a design doc under
   `docs/design_docs/` using `design_template.md`. Get approval before implementing.
2. **Implement** — follow the coding style below. Keep edits minimal and scoped.
3. **Test** — add or update tests colocated with the source module. Every test
   runs in both SIMD native and scalar modes automatically.
4. **Format** — run `clang-format` on changed C++ files and `buildifier` on
   changed Bazel files.
5. **Gate** — both commands must pass before any commit:
   ```bash
   bazel build //...
   bazel test //...
   ```

## Coding Style

- **C++20** standard; Bazel-first build system.
- **Line length**: 100 characters max.
- **Naming**: `lowerCamelCase` for functions, `UpperCamelCase` for types.
- **Formatting**: Google style via `.clang-format` (run `clang-format -i <file>`).
- **Floating point**: always compile with `-ffp-contract=off` for reproducible results
  across SIMD backends (already set in both Bazel and CMake configs).
- Prefer deterministic, reproducible implementations.
- Keep comments to non-obvious logic only.

## Adding Tests

Tests are colocated with source modules:

```
src/tiny_skia/Foo.cpp       → src/tiny_skia/tests/FooTest.cpp
src/tiny_skia/bar/Baz.cpp   → src/tiny_skia/bar/tests/BazTest.cpp
```

Use the `tiny_skia_dual_mode_cc_test` macro in BUILD files to automatically
create native and scalar test variants:

```bzl
load("//bazel:simd_test.bzl", "tiny_skia_dual_mode_cc_test")

tiny_skia_dual_mode_cc_test(
    name = "foo_test",
    srcs = ["tests/FooTest.cpp"],
    deps = ["//src:tiny_skia_lib"],
)
```

Prefer Google Mock matchers (`EXPECT_THAT`, `ASSERT_THAT`) for container and
value assertions. Reusable matchers live in `src/**/tests/test_utils/`.

## Adding a New Module

1. Create `src/tiny_skia/newmod/Foo.h` and `Foo.cpp`.
2. Add a `BUILD.bazel` in the new directory with a `cc_library` target.
3. Wire the new target into `src/BUILD.bazel` dependencies.
4. Add source files to `CMakeLists.txt`.
5. Create `src/tiny_skia/newmod/tests/FooTest.cpp` with a dual-mode test target.
6. Run `bazel build //...` and `bazel test //...`.

## Formatting

```bash
# Format C++ files
clang-format -i src/tiny_skia/Foo.cpp src/tiny_skia/Foo.h

# Format Bazel files
buildifier src/tiny_skia/BUILD.bazel
```

## Troubleshooting

### Bazel version mismatch

The project requires Bazel 7+. If you see version errors, install Bazelisk:
```bash
./tools/env-setup.sh
```

### C++20 compilation errors

Ensure your compiler supports C++20. The `.bazelrc` sets `--cxxopt=-std=c++20`.
For CMake, the `CMakeLists.txt` requires `cxx_std_20`.

### Floating-point result differences

All builds use `-ffp-contract=off` to ensure reproducibility across SIMD
backends. If you see unexpected floating-point mismatches, verify this flag is
present in your build configuration. Note that the C++ port is not bit-exact
with the Rust original due to additional features and optimizations; golden-image
tests use configurable tolerance thresholds.

### Test failures after SIMD changes

Any change to `wide/` types must produce identical results in both native and
scalar modes. Run the full dual-mode test suite to verify:
```bash
bazel test //...
```

The cross-validation tests (`tests/integration/cross_validation_test`) compare
C++ output pixel-perfectly against the original Rust tiny-skia via FFI.
