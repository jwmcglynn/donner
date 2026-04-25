# wgpu-native Cargo crate-universe lockfile

Holds the `Cargo.Bazel.lock` + pre-generated `bindings.rs` that
`rules_rust`'s `crate_universe` extension reads when building the
patched `@wgpu_native_source` from source. The source build is
unconditional on native platforms (macOS + Linux) because the
`wgpuDeviceGetMetalRawDevice` / `wgpuDeviceCreateTextureFromIOSurface`
exports added by `//third_party/patches:wgpu-native-iosurface-export.patch`
need to be part of the wgpu-native ABI always — the editor sandbox's
cross-process texture bridge is not a feature flag.

## Files

- `Cargo.Bazel.lock` — resolver output consumed by `crate.from_cargo`
  in `//MODULE.bazel`. Regenerate with
  `CARGO_BAZEL_REPIN=1 bazel fetch @wgpu_native_cargo//...` after
  bumping the `wgpu_native_source` tag in
  `//third_party/bazel/non_bcr_deps.bzl`.
- `bindings.rs` — pre-generated output of wgpu-native's `build.rs`
  (which runs `bindgen` against `ffi/wgpu.h`). Shipped here because
  running `bindgen` under Bazel requires `libclang` on the host path,
  which is fragile across dev machines and remote-execution workers.
  The `//third_party/patches:wgpu-native-iosurface-export.patch`
  rewrites `src/lib.rs`'s `include!(concat!(env!("OUT_DIR"), "/bindings.rs"))`
  to `include!("bindings.rs")` so the rust crate picks this file up
  via `src/bindings.rs` staged by the `bindings_rs` `genrule` in
  `//third_party:BUILD.wgpu_native_source`.
  Regenerate with
  `cd $(bazel info output_base)/external/+non_bcr_deps+wgpu_native_source && \
   CARGO_TARGET_DIR=/tmp/wgpu-target cargo build --release --features metal && \
   cp /tmp/wgpu-target/release/build/wgpu-native-*/out/bindings.rs \
      //third_party/bazel/wgpu_native_cargo/bindings.rs`.

## Rebuild checklist after a tag bump

1. Update `tag =` in `//third_party/bazel/non_bcr_deps.bzl`.
2. Re-apply / regenerate the patch if the upstream source drifted.
3. `CARGO_BAZEL_REPIN=1 bazel fetch @wgpu_native_cargo//...` to
   refresh `Cargo.Bazel.lock`.
4. Regenerate `bindings.rs` per the command above.
5. `bazel build --config=geode //donner/editor:editor` to verify.

See `docs/design_docs/0023-editor_sandbox.md` §"Rust-side wgpu-native
patch" for the overall plan.
