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
- `rust_dependencies.json` - Rust sources and Cargo metadata partitioned by
  scope (inert reference snapshot, test-only cross-validation oracle, active),
  Rust build-rule references, and Rust-built prebuilt archive declarations.
  `activeRustSources` is the section that must stay empty.

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
python3 tools/gpu_inventory/check_no_rust_dependencies.py                     # report
python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking default  # as CI
python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking          # all
python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking a,b      # some
```

The rule is a closure property: no Rust compiler invocation, Cargo execution, or
Rust-built library in a shipped artifact or a non-test dependency closure. Rust
in the tree is not itself the violation, so `rust_allowlist.json` names three
scopes and the verifier enforces the boundary of each:

- `inertReferencePrefixes` - upstream reference source no build rule may compile
  or link. Staging its golden images through `data` is fine; naming it from
  `srcs`/`deps`/`hdrs` is a finding.
- `testOnlyRustPrefixes` - the tiny-skia cross-validation oracle and the
  vendored workspace `MODULE.bazel` that declares its Rust toolchain. That
  workspace is a `local_repository` repo rule and is hidden by `.bazelignore`,
  so Donner's module graph never evaluates it and a clean checkout builds and
  tests without `rustc`. Rust source and `rules_rust` are allowed here only.
- `testOnlyConsumerPrefixes` - the only build files that may name the oracle's
  targets. This is what keeps it out of every non-test closure, and it is
  checked alongside the oracle's own visibility.

The visibility check reads the raw file and fails closed on anything it cannot
parse as a literal list of quoted labels, a comment included: a comment
containing `visibility = ...` in one of these BUILD files is reported as a
non-literal visibility. Reword the comment rather than loosening the check. The
only labels it accepts are `__pkg__` and `__subpackages__` targets under the
vendored `//tests` tree, so a package_group label, whose membership is declared
elsewhere, is a finding.

The Lint workflow runs `--blocking default`, which is every category except
`rust-built-archive`: the prebuilt `wgpu-native` tarballs are the Rust that
actually ships today, and they leave with the Metal and Linux cutovers.

Bazel files are scanned for Rust rule-set names and, outside comments, for bare
`cargo`, `rustc`, and `rustup` commands, because a `genrule` command or a `.bzl`
action can run the toolchain without naming a Rust rule.

CMake needs a different list (`cargo`, `corrosion`, `rustc`, `find_package(Rust`)
and two gates, because Donner's production `CMakeLists.txt` files are emitted by
`tools/cmake/gen_cmakelists.py` and are git-ignored:

- **This verifier**, in the Lint job, scans the tracked CMake files (vendored and
  hand-written) and the tracked generator sources under `tools/cmake/`, which is
  where an emitted Rust command has to be written first. `*_test.py` there is
  excluded: it emits nothing and its fixtures hold these strings on purpose.
- **`gen_cmakelists.py --check`**, in the cmake-validate job, generates the real
  output and scans it with the same list. That is the only gate that reads the
  emitted files, because they do not exist in a fresh checkout.

Neither gate sees an emitted file on a developer machine that never ran the
generator; between them they cover the tracked source and the generated output
in CI.

The generator-source scan does not exempt comments, because a string that
reaches the emitted output is indistinguishable from prose to a lexical check.
`gen_cmakelists.py` therefore avoids writing these words in its own prose and
shares the token list with this verifier instead of restating it.

The scan reads git-tracked files only. A generated `MODULE.bazel.lock` and a
generated root `CMakeLists.txt` are both out of scope: the lockfile records the
whole transitive Bzlmod graph, including the `rules_rust` that protobuf declares
and nothing fetches, which is the graph rather than the closure.

Bazel is the second defense and does not depend on this tool. From the Donner
root, `bazel query 'deps(@tiny-skia-cpp//tests/rust_ffi:tiny_skia_ffi)'` fails
to load, because `@rules_rust` is not visible from a repository pulled in with a
repo rule instead of as a module. A Donner target that reaches the oracle fails
at load time rather than silently linking Rust, and adding `rules_rust` to the
root module graph to make it load is itself a `rust-build-edge` finding.
