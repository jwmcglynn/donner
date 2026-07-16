# GPU runtime inventory (design doc 0053)

Machine-readable manifests of Donner's current GPU surface, plus the
no-Rust-dependency verifier. See
[docs/design_docs/0053-native_gpu_hal.md](../../docs/design_docs/0053-native_gpu_hal.md).

## Manifests

`manifests/*.json` is generated from the git-tracked tree:

```sh
python3 tools/gpu_inventory/generate_gpu_manifests.py          # regenerate
python3 tools/gpu_inventory/generate_gpu_manifests.py --check  # CI freshness gate
```

- `gpu_operations.json` - per-file WebGPU wrapper/C-API tokens and GPU operation
  methods used under `donner/`.
- `shader_features.json` - WGSL shaders (files and inline literals): entry
  points, bindings, builtins, language features.
- `editor_integration.json` - editor files touching the GPU surface, including
  the ImGui WebGPU backend.
- `rust_dependencies.json` - Rust sources and Cargo metadata (allowlisted inert
  reference vs active), Rust build-rule references, and Rust-built prebuilt
  archive declarations.

New GPU operations or shader features cannot land without updating the
manifests: the Lint workflow runs `--check`, which fails when a regeneration
differs from the checked-in files.

## No-Rust-dependency verifier

```sh
python3 tools/gpu_inventory/check_no_rust_dependencies.py             # report-only
python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking  # phase 6 gate
```

Scans tracked files plus generated build state (`MODULE.bazel.lock`) for Rust
dependency edges outside the inert reference allowlist
(`rust_allowlist.json`). Report-only during the design 0053 transition; phase 6
switches the Lint workflow step to `--blocking`.
