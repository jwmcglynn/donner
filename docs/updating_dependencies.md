# Updating Dependencies {#UpdatingDependencies}

Pin every third-party dependency to an upstream release tag or version, never to a commit.
Renovate follows releases, and for a dependency it tracks, a commit pin makes it propose every
upstream commit instead. Bazel modules take `bazel_dep` versions from the Bazel Central Registry.
A dependency fetched outside the registry, in `third_party/bazel/non_bcr_deps.bzl`, names its
release tag or release asset; `renovate.json` ignores `third_party/`, so those fetches are bumped
by hand under the same rule. When one also needs a content pin, fetch the release's published
asset, or its tag source archive if it publishes none, with its `sha256` rather than replacing the
tag with a commit.

A few older pins still name a commit: the `git_override` blocks for `hedron_compile_commands`,
`bloaty`, `imgui` and `glfw` in `MODULE.bazel` and for `stb`, `imgui` and `glfw` in
`examples/MODULE.bazel`, `woff2` and `bazel_clang_tidy` in `third_party/bazel/non_bcr_deps.bzl`,
and the Google Fonts files in `third_party/google_fonts/fonts.bzl`. The CMake generator,
`tools/cmake/gen_cmakelists.py`, also fetches every dependency by commit: googletest,
nlohmann_json, zlib, rules_cc and pixelmatch-cpp17 by the commit recorded for the release
`MODULE.bazel` selects, and the generator fails if `MODULE.bazel` moves to another release; absl,
EnTT and brotli by fixed revisions; and woff2 by the commit `non_bcr_deps.bzl` names. These predate
this rule; do not copy them, and ask the operator before adding a dependency that has no usable
release.

## clang-format

The C++ formatter is clang-format from the PyPI `clang-format` wheels, one per supported platform
(`clang_format_*` in `third_party/bazel/non_bcr_deps.bzl`), each pinned by its version-named file
and `sha256`. To move to another release:

1. Update every wheel's path and `sha256` from the release's PyPI file list, and the version in
   the URL template and comment.
2. Update `tools/format/clang_format_fixture_test.py` and
   `tools/format/testdata/divergent_constructs.formatted.cc.in` for the new version's output.
3. Reformat the repository with `tools/format/clang_format.sh -i` in the same change, so
   `bazel test //...` and the Lint lane pass, and afterwards add the resulting main commit to
   `.git-blame-ignore-revs`.

## ESLint

`//donner/editor/wasm/tests:js_lint_test` runs ESLint with the rules CodeFactor applies, so a
CodeFactor finding fails `bazel test //...` first. `eslint` and `@eslint/js` are pinned in
`donner/editor/wasm/tests/package.json` to the ESLint release that CodeFactor lists for its
analysis, and `globals` to the version that release's environments use. Renovate leaves these
three alone. When CodeFactor moves to another release, or changes its default configuration
(`.eslintrc.json` in `codefactor-io/default-configs`):

1. Update the versions in `package.json`, then regenerate `package-lock.json`
   (`npm install --package-lock-only --ignore-scripts`) and `pnpm-lock.yaml`
   (`pnpm install --lockfile-only --ignore-scripts`, pnpm 9 or later) in that directory.
2. Mirror any rule or environment change in `donner/editor/wasm/tests/eslint.config.mjs`, and
   fix new findings in the same change.

## Bazel LLVM Toolchain

Donner uses `toolchains_llvm` from the Bazel Central Registry. To update it, bump its version in
`MODULE.bazel`:

```py
bazel_dep(name = "toolchains_llvm", version = "...", dev_dependency = True)
```

A `single_version_override` after it applies
`third_party/patches/toolchains_llvm-libcxx-compile-headers.patch` to whichever release the
`bazel_dep` resolves to. It sets no version, so a version bump keeps the patch. If a new release no
longer accepts the patch, the bump fails while fetching the module; refresh the patch against that
release. Only Bazel releases before 9 read the filegroup it changes.

The LLVM release itself is selected by `llvm_version` in each `llvm.toolchain(...)` call that
follows: the host toolchain and the two Linux exec-platform toolchains. Keep all three on the same
release.

To test changes to `toolchains_llvm` locally, clone it next to the `donner` directory, comment out
the `single_version_override` (a module takes only one override, and the local copy does not get the
patch), and uncomment the `local_path_override` block below it in `MODULE.bazel`:

```py
local_path_override(
    module_name = "toolchains_llvm",
    path = "../toolchains_llvm",
)
```
