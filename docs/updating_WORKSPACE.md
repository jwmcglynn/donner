# Updating the WORKSPACE

## Skia version

There's a fork of the skia library at https://github.com/jwmcglynn/skia, which contains fixes for build errors compared to the upstream version.

To pull a new version, in a local clone of the https://github.com/jwmcglynn/skia repo, run:

```bash
  git fetch upstream
  git checkout -b new_upstream upstream/main
```

To test it against a local Donner WORKSPACE, modify the `skia` repository to point to your local skia repo:

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

```
bazel build //...
bazel test //...
```
