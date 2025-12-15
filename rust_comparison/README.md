# Rust Comparison Workspace

This subworkspace loads the Donner module via `local_path_override` and layers
`rules_rust` toolchains only for tiny-skia Rust parity checks. It keeps Rust
rules out of the main Donner Bazel module while making it easy to compile or
reference the vendored Rust crate during comparisons.

To use it:

1. Run Bazel from this directory so the local module is picked up:
   ```bash
   cd rust_comparison
   bazel info
   ```
2. Add Rust comparison targets under this tree and depend on `@donner//...`
   for the C++ port artifacts. The `rules_rust` toolchains are registered via
   the module extension in this workspace.
3. Run the combined C++/Rust canvas comparison test, which renders the same
   scene in both languages via a small FFI layer and compares the outputs with
   the shared `ImageComparisonTestFixture` (pixelmatch-cpp17):
   ```bash
   bazel test //:tiny_skia_parity_tests
   ```
   The test keeps Rust-only dependencies in this workspace while exercising
   the C++ tiny_skia_cpp backend from the main Donner module. Current guardrails
  allow up to 7k mismatched pixels at a 0.01 threshold; failures emit expected,
   actual, and diff PNGs for debugging.

4. Build the Rust parity harness that emits a tiny-skia PNG for side-by-side
   checks against the C++ goldens when you only need the Rust output:
   ```bash
   bazel run //:tiny_skia_rust_png -- /tmp/rust_reference.png
   ```
   The harness renders a simple gradient-filled path with a stroked outline
   so renderings can be compared without adding Rust dependencies to the main
   Donner module. It pulls `tiny-skia` 0.11.4 from crates.io, matching the
   vendored crate version in `third_party/`.
