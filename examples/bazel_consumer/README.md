# Bazel consumer smoke test

This separate module parses and renders an SVG through Donner's public API using the default
tiny-skia backend. Its own `.bazelrc` selects C++20. Donner's root configuration and development-only
module dependencies do not apply.

`BCR Preflight` resolves the pinned Donner version from a disposable registry backed by the exact
candidate source archive. The version in this fixture must match the root `MODULE.bazel`.

For an adjacent checkout during development, run from this directory:

```sh
bazel test --override_module=donner=../.. //:render_svg
```

For an already-published version, set the `bazel_dep` version and run `bazel test //:render_svg`
without the override. The BCR test-module mechanism supplies its own candidate module override.
A checkout test alone does not verify packaging or registry admission; see the
[BCR release runbook](../../docs/design_docs/0018-bcr_release.md).
