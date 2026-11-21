# HarfBuzz OpenType language data

This package carries the forward BCP 47 to OpenType language-system mapping needed by the
basic text backend. The generated data is included once by the SVG text system. It does not
link the HarfBuzz library. The owning license is reproduced in [COPYING](COPYING).

To refresh the data, unpack the HarfBuzz archive pinned by
`third_party/bazel/non_bcr_deps.bzl` and run
`python3 third_party/harfbuzz-language-data/generate_language_data.py /path/to/harfbuzz`.
Run the same command with `--check` to verify the generated file and copied license.
