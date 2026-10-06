# No-Rust production boundary

## Configured no-Rust dependency audits

Ordinary `configured_dependency_audit_test` targets follow selected Bazel
dependencies, aliases, Wasm wrappers, additional linker inputs, and linker
options through build transitions. The audits live beside the native library,
CLI, editor, Geode embed, renderer, and browser package roots they protect.
Production roots reject the complete WebGPU-C++/wgpu-native reference package
and the vendored Rust FFI oracle. Required labels keep every audit non-vacuous.

The Linux resvg comparison has the inverse positive contract: its audit requires
the complete test-only renderer, wrapper, alias, and architecture-selected
archive chain. `//tools/ci:linux_wgpu_resvg_reference` runs that audit with the
oracle. Editor Wasm CI runs the shipped browser audits in their owning
configuration. Native and transitioned Geode audits run as normal Bazel tests,
so the standard platform suites bind them to the source and selected graph they
actually build.

Generated CMake remains a separate build surface. `gen_cmakelists.py --check`
rejects Rust toolchain commands and any install rule until an install-payload
scanner exists; the CMake workflow also builds and runs the public consumer.
CLI and browser artifact construction, integrity, and size stay in their
existing BCR and Editor Wasm workflows rather than a second dependency receipt.

## No-Rust-dependency verifier

```sh
python3 tools/rust_boundary/check_no_rust_dependencies.py                     # report
python3 tools/rust_boundary/check_no_rust_dependencies.py --blocking default  # as CI
python3 tools/rust_boundary/check_no_rust_dependencies.py --blocking          # all
python3 tools/rust_boundary/check_no_rust_dependencies.py --blocking a,b      # some
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
- `testOnlyGpuOracleArchives` - the exact Linux aarch64 and x86_64 wgpu-native
  release assets and reviewed SHA-256 pins for the sole resvg comparison lane.
  The verifier checks the fetch rule, root module names, Linux-only overlay,
  test-only wrapper alias chain, and resvg test consumer as one narrow declared
  boundary. It also pins the complete set of first-party rules the archive is
  reachable from: the wrapper's `webgpu_cpp`, two aliases and reference
  runtime, the two Geode reference leaves, the resvg comparison's libraries,
  test, wrapper and audit, and the CI `test_suite` that selects them. Every
  string literal a Starlark build file names outside dependency-audit metadata
  is read as a label in that file's package; one that resolves to a pinned rule,
  to a target a pinned macro generates (an audit's `_checker`, a transitioned
  test's `_ci_remote`, a cc test's `.stripped` and `.dwp` outputs and variant
  wrappers) or to an archive may appear only in that rule's own declaration. So any other
  rule, in those files or elsewhere, that names the chain fails, as does a
  pinned rule that disappears or changes kind. A label assembled from pieces is
  not read. Bazel visibility narrows the wrapper, the Geode leaves and the
  comparison's libraries, but the archive repositories and `//tools/ci` are
  publicly visible, so for those hops this check is the only guard. The
  full tracked-tree scan also fails if any of these boundary files disappears.
  The Linux oracle's configured dependency audit proves the selected test root
  actually reaches that archive.

The visibility check reads the raw file and fails closed on anything it cannot
parse as a literal list of quoted labels, a comment included: a comment
containing `visibility = ...` in one of these BUILD files is reported as a
non-literal visibility. Reword the comment rather than loosening the check. The
only labels it accepts are `__pkg__` and `__subpackages__` targets under the
vendored `//tests` tree, so a package_group label, whose membership is declared
elsewhere, is a finding.

The Lint workflow runs `--blocking default`, which includes
`rust-built-archive`. The two pinned Linux test-oracle archives are its only
exception; an extra archive, production reference, or missing test-only guard
fails. Bazel files are scanned for Rust rule-set names and, outside comments, for bare
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

GPU, shader, renderer, editor and browser behavior is checked by executable tests.
