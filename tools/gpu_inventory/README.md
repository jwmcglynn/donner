# GPU runtime inventory (design doc 0053)

Machine-readable manifests of Donner's current GPU surface, plus the
no-Rust-dependency verifier. See
[docs/design_docs/0053-native_gpu_hal.md](../../docs/design_docs/0053-native_gpu_hal.md).

## Manifests

`manifests/*.json` records the **semantic inventory** of the GPU surface: which
files use which GPU operations, what each shader declares, which editor files
touch the GPU, and where Rust enters the build graph. Nothing else - no content
hashes, no line counts, nothing derived from a file's bytes beyond those facts.

That distinction is the whole design. Entries used to carry a `sha256` of each
scanned file, which meant any edit to a GPU-using file - a comment, a rename, a
reordered include - made the manifests stale and failed CI until someone
committed a mechanical regeneration. Now an edit that does not change what the
code does with the GPU changes nothing here and needs no regeneration.

Freshness is checked by `//tools/gpu_inventory:manifest_freshness_tests` under
plain `bazel test //...`. It rescans its declared inputs and diffs the result
against the checked-in manifests, naming the file and the changed fact.

Regenerate when the inventory legitimately changes:

```sh
python3 tools/gpu_inventory/generate_gpu_manifests.py
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
manifests: `manifest_freshness_tests` fails when a rescan differs from the
checked-in files.

### Scan inputs

The freshness test does not walk the source tree and never calls git; its inputs
are the per-package `gpu_inventory_srcs` filegroups, so Bazel can cache it and
change-based target selection can see which edits matter. **A package that gains
GPU code needs that filegroup**; `test_every_inventoried_file_is_a_declared_input`
fails loudly if a manifest names a file the test cannot see, rather than
silently scanning a smaller tree than the manifests describe.

`rust_dependencies.json` is the one manifest not held to exact equality by the
test: most of its content is the vendored `third_party/tiny-skia-cpp` subtree,
which `.bazelignore` hides from Bazel entirely. It gets a one-directional
ratchet instead - no new Rust build edge or Rust-built archive site may appear
among the files Bazel does own - backed by the no-Rust-dependency verifier
below and by the regeneration command.

The scan stays deliberately lexical and over-approximating: it matches tokens
rather than parsing C++ or WGSL, including inside comments and strings, because
a false positive costs a manifest line and a false negative costs the ratchet.
The `OPERATION_METHODS` table in the generator is now the ratchet's backstop
(the content hash used to be), so add an entry there when the backend starts
calling a new WebGPU method.

## No-Rust-dependency verifier

```sh
python3 tools/gpu_inventory/check_no_rust_dependencies.py             # report-only
python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking  # phase 6 gate
```

Scans tracked files plus generated build state (`MODULE.bazel.lock`) for Rust
dependency edges outside the inert reference allowlist
(`rust_allowlist.json`). Report-only during the design 0053 transition; phase 6
switches the Lint workflow step to `--blocking`.
