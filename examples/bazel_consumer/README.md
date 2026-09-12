# Bazel consumer smoke test

This separate module parses and renders an SVG through Donner's public API using the default
tiny-skia backend. It consumes the adjacent source tree as a dependency, so Donner's development
module dependencies and root `.bazelrc` do not apply. Its own `.bazelrc` selects the required C++20
language mode.

From this directory, run `bazel test //:render_svg`. For a registry release, remove the
`local_path_override` and add the published version to `bazel_dep`.
