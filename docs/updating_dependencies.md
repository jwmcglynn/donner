# Updating Dependencies {#UpdatingDependencies}

## Bazel LLVM Toolchain

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

