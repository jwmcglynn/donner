# Updating Dependencies {#UpdatingDependencies}

Pin every third-party dependency to an upstream release tag or version, never to a commit.
Renovate follows releases, and a commit pin makes it propose every upstream commit instead. Bazel
modules use `bazel_dep` versions from the Bazel Central Registry. A dependency fetched outside the
registry, in `third_party/bazel/non_bcr_deps.bzl`, names its release tag; when it also needs a
content pin, fetch the release's tag archive or published release asset with its `sha256` rather
than replacing the tag with a commit.

## Bazel LLVM Toolchain

Donner uses `toolchains_llvm` from the Bazel Central Registry. To update it, bump its version in
`MODULE.bazel`:

```py
bazel_dep(name = "toolchains_llvm", version = "1.8.0", dev_dependency = True)
```

The LLVM release itself is selected by `llvm_version` in the `llvm.toolchain(...)` call that
follows.

To test changes to `toolchains_llvm` locally, clone it next to the `donner` directory and uncomment
the `local_path_override` block below the `bazel_dep` in `MODULE.bazel`:

```py
local_path_override(
    module_name = "toolchains_llvm",
    path = "../toolchains_llvm",
)
```
