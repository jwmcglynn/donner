# Curated Google Fonts (embedded, build-time fetch)

Design 0013 W3. A curated starter set of 12 open-licensed families spanning
sans / serif / display / mono / script, embedded into the Donner editor for the
font picker and for `TextEngine` resolution of documents that name these
families.

Unlike `third_party/roboto`, `third_party/fira-code`, and
`third_party/public-sans` (whose `.ttf`/`.otf` bytes are checked in), these font
files are **not** committed. They are fetched at build time by pinned-hash
`http_file` repos declared in `fonts.bzl` (`GOOGLE_FONTS`), so:

- builds are deterministic (each URL is commit-pinned to `google/fonts` and
  guarded by a `sha256`);
- they work offline after the first fetch (Bazel's repository cache);
- the embedded bytes are plain C arrays, so wasm builds are unaffected.

## Adding or changing a family

1. Pick a file from a specific `google/fonts` commit (raw.githubusercontent URL).
2. Download it once and compute `shasum -a 256`.
3. Add a `struct(...)` entry to `GOOGLE_FONTS` in `fonts.bzl` and register the
   new `repo` name in `//MODULE.bazel` (`use_repo(google_fonts, ...)`).
4. `//third_party/google_fonts:integrity_test` validates the pin table shape.

## Licensing

Every family in the set is under the SIL Open Font License 1.1 (OFL). The OFL
permits embedding and redistribution. `LICENSES.md` records the per-family
license and upstream path; the pinned URLs point at the exact upstream files.
