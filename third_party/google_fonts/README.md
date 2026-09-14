# Curated Google Fonts

A curated set of 12 open-licensed families spans sans, serif, display, mono,
and script fonts for the editor font picker and document text.

Unlike `third_party/roboto`, `third_party/fira-code`, and
`third_party/public-sans` (whose `.ttf`/`.otf` bytes are checked in), these font
files are **not** committed. They are fetched at build time by pinned-hash
`http_file` repos declared in `fonts.bzl` (`GOOGLE_FONTS`), so:

- each source is commit-pinned to `google/fonts` and guarded by a SHA-256;
- generation works offline after the first fetch into Bazel's repository cache;
- the pinned Google WOFF2 encoder and Brotli produce one encoded catalog for
  both native embedding and deferred web delivery.

## Generated interfaces

`google_fonts_catalog_inc` exposes payload-free metadata through
`embed_resources/GoogleFontsCatalog.inc`. Its macro has seven arguments:
`DONNER_GF_ENTRY(family, category, sha256, path, encoded_bytes, decoded_bytes, symbol)`.
`symbol` is an unqualified token, so web consumers can discard it without
referencing the native arrays.

`google_fonts_data` contains immutable WOFF2 arrays and their
`donner::embedded::kGF<Family>Woff2` spans in
`embed_resources/GoogleFontsData.h`. Native editors link this target and use the
existing full-text decoder in memory. Metadata consumers do not depend on this
library.

`google_fonts_woff2_assets` supplies a directory containing
`fonts/<sha256>.woff2`, `catalog-fonts.json`, and `CatalogFontNotices.txt`.
Packaging must preserve these relative paths. The manifest records exact source
pins, encoder versions and settings, encoded and decoded hashes and lengths,
and license provenance. Runtime code uses compiled metadata as its authority.

The editor's automatic NOTICE aggregation embeds the same complete notices independently of the
font payloads; `catalog_notices` exposes the plain text to other notice tooling.

Generation retains the full glyph repertoire, variation axes, names and layout
tables. It does not subset fonts or freeze axes. WOFF2 may normalize glyph
encoding and offsets, update sfnt checksums and the lossless-transform flag, and
remove an invalidated digital signature. Every other table remains byte-identical.
An asset may occupy at most 2 MiB encoded and expanded, and the encoded catalog
must fit the 4 MiB retention budget.

The generator also enforces the catalog decoder's standalone TrueType contract:
at most 64 tables, 2 MiB total Brotli output, and 512 KiB transformed `glyf` data.
Its roundtrip uses the same 8 MiB bounded Brotli allocator as catalog rendering.
The manifest fingerprints the configured WOFF2 sources, including local patches.

The controlled loading allocation bound is:

```
decode = max_per_font(encoded + decoded)
       + max(2 MiB + max(8 MiB, 29 * 512 KiB + 2 MiB), 4 MiB)
broker = 8 * (largest_encoded + second_largest_encoded)
total  = decode + broker + 2 MiB
```

The decoder term includes one provider vector, exact decoded storage, intermediate
bytes, Brotli or glyph reconstruction scratch (which run sequentially), and sfnt
validation workspace. The broker term covers two concurrent loaders' owned
stream/hash/transfer/promotion copies. The last 2 MiB covers one older persistent
cache body during the single allowed write/trim operation. Generation fails above
32 MiB and records every constant and calculated component in the manifest.
Encoded retention, consumer-retained decoded fonts, scene dependency graphs, nested
render surfaces, and browser-owned network or storage internals are separate from
this controlled allocation bound.

## Verification

`//third_party/google_fonts:catalog_woff2_assets_test` includes pin, metadata,
generator-boundary and per-family roundtrip tests. Each family is independently
encoded twice and compared with the packaged content address. The tests compare
all preserved tables, character maps, and every glyph's outline and metrics at
the original default instance and representative variable-axis endpoints.

## Adding or changing a family

1. Pick a file from a specific `google/fonts` commit (raw.githubusercontent URL).
2. Download it once and compute `shasum -a 256`.
3. Add a `struct(...)` entry to `GOOGLE_FONTS` in `fonts.bzl` and register the
   new `repo` name in `//MODULE.bazel` (`use_repo(google_fonts, ...)`).
4. Keep `fonts_manifest_reference.json` and the original upstream OFL notice in
   `licenses/` aligned with the new pin.
5. Run the complete catalog asset test suite and the native/full-text rendering
   tests. Review any content hash or encoder pin change before distribution.

## Licensing

Every family in the set is under the SIL Open Font License 1.1 (OFL). The OFL
permits embedding and redistribution. `LICENSES.md` records the per-family
license and upstream path. The unmodified notices under `licenses/` come from
the same pinned revision and are included in native and web distribution.
