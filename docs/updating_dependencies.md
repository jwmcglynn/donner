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
`examples/MODULE.bazel`, and `woff2` and `bazel_clang_tidy` in
`third_party/bazel/non_bcr_deps.bzl`. They predate this rule; do not copy them, and ask the
operator before adding a dependency that has no usable release.

## Bazel LLVM Toolchain

Donner uses `toolchains_llvm` from the Bazel Central Registry. To update it, bump its version in
`MODULE.bazel`:

```py
bazel_dep(name = "toolchains_llvm", version = "1.8.0", dev_dependency = True)
```

The LLVM release itself is selected by `llvm_version` in each `llvm.toolchain(...)` call that
follows: the host toolchain and the two Linux exec-platform toolchains. Keep all three on the same
release.

To test changes to `toolchains_llvm` locally, clone it next to the `donner` directory and uncomment
the `local_path_override` block below the `bazel_dep` in `MODULE.bazel`:

```py
local_path_override(
    module_name = "toolchains_llvm",
    path = "../toolchains_llvm",
)
```
