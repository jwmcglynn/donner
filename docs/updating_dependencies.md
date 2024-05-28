# Updating Dependencies

## Bazel llvm_toolchain

Donner uses a modified version of llvm_toolchain to prototype libclang-based tools (such as the archived [hdoc](https://github.com/jwmcglynn/donner/blob/hdoc-archive/docs/devtools/generating_documentation.md) experiment, in the `hdoc-archive` branch).

To update to a new version, modify `MODULE.bazel`:

```py
git_override(
    module_name = "toolchains_llvm",
    remote = "https://github.com/jwmcglynn/toolchains_llvm",
    commit = "ac1836bde88f7a55e3d6c0161844d98ae9bf861e",
)
```

To locally test changes, assuming repro is cloned locally next to the `donner` directory:

```diff
- git_override(
-     module_name = "toolchains_llvm",
-     remote = "https://github.com/jwmcglynn/toolchains_llvm",
-     commit = "ac1836bde88f7a55e3d6c0161844d98ae9bf861e",
- )
+ local_path_override(
+     module_name = "toolchains_llvm",
+     path = "../toolchains_llvm",
+ )
```

## Skia version

There's a fork of the skia library at https://github.com/jwmcglynn/skia, which contains fixes for build errors compared to the upstream version.

To pull a new version, in a local clone of the https://github.com/jwmcglynn/skia repo, run:

```bash
git fetch upstream
git checkout -b new_upstream upstream/main
```

To test it against a local Donner MODULE.bazel, modify the `skia` repository to point to your local skia repo:

```diff
- git_repository(
-     name = "skia",
-     commit = "aaf2f8dd2bba1260395c90635d83df271e753fbd",
-     remote = "https://github.com/jwmcglynn/skia",
- )
+ local_repository(
+     name = "skia",
+     path = "/path/to/skia",
+     # For example:
+     # path = "../skia",
+ )
```

Then build Donner and verify if it works.

```sh
bazel build //...
bazel test //...
```
