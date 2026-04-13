# Text Rendering: Overview

[Back to hub](../0010-text_rendering.md)

**Status:** Implemented (base + text_full tiers)
**Author:** Claude Opus 4.6
**Created:** 2026-03-09
**Tracking:** [#242](https://github.com/jwmcglynn/donner/issues/242)

## Summary

Donner ships a two-tier text stack:

1. **Base text** (`text`) — stb_truetype-based shaping/layout for common SVG text.
2. **Full text** (`text_full`) — HarfBuzz + FreeType for complex scripts, bitmap emoji,
   WOFF2, and native OpenType shaping.

Both tiers share a single `TextEngine` with pluggable backends
(`TextBackendSimple` / `TextBackendFull`). Skia and TinySkia both render text today.

This design covers:

1. **Base text support** — common `<text>` / `<tspan>` rendering, positioning, decoration,
   `textLength`, `lengthAdjust`, and `@font-face`.
2. **Shared layout architecture** — one `TextEngine`, one `TextBackend` interface, no duplicated
   layout logic between base and full tiers.
3. **Optional advanced shaping** — HarfBuzz/FreeType/WOFF2 only when `text_full` is enabled.
4. **Feature tiers** — build-time opt-in via Bazel flags controlling binary size cost.

Full OpenType shaping (GSUB/GPOS via HarfBuzz) is available as an opt-in `text_full` tier
(`--config=text-full`) to avoid roughly doubling the binary size when not needed.

## Goals

- Render `<text>` and `<tspan>` elements correctly for common SVG content on both backends.
- Support `@font-face` with TTF/OTF and WOFF 1.0 web fonts.
- Add zero new dependencies for the base text tier (stb_truetype is already vendored).
- Keep binary size and build time impact minimal and opt-in via feature tiers.
- Keep the architecture shared so HarfBuzz shaping remains a drop-in upgrade.
- Maintain build-time backend selection.

## Non-Goals

- Full bidirectional text (mixed LTR/RTL in a single `<text>` element via the Unicode Bidi algorithm).
- Full mixed-script vertical writing mode parity.
- Complete `<textPath>` parity (`method`, `spacing`, `side`, `path` attribute, effect interactions).
- SVG fonts (`<font>` element — deprecated in SVG2).
- Font subsetting or optimization.
- System font discovery for TinySkia (embedded/loaded fonts only).

## Feature Tiers {#feature-tiers}

Text support is split into independently selectable features, ordered by dependency cost:

| Feature | What it adds | New deps | Binary cost | Status |
|---------|-------------|----------|------------|--------|
| **`text`** | Font loading (TTF/OTF/WOFF1), `kern`-table kerning, glyph outlines, `@font-face` | None (stb_truetype already vendored) | ~55 KB | **Done** |
| **`text_full`** | Full OpenType shaping (GSUB/GPOS: ligatures, contextual kerning), WOFF2 web font support | HarfBuzz (`HB_TINY`), Google woff2 + Brotli | ~500-800 KB | **Done** |

### What the base `text` tier provides vs full text shaping

The base tier uses stb_truetype's `kern` table for pair kerning and `stbtt_GetGlyphHMetrics()`
for glyph advances. This covers:

- **Correct glyph advances** — characters are spaced according to the font's horizontal metrics.
- **Pair kerning** — common kern pairs (AV, To, Wa, etc.) are adjusted via the `kern` table.
- **Glyph outlines** — `stbtt_GetGlyphShape()` extracts Bezier curves for path-based rendering.

What it does **not** cover (deferred to `text_full`):

- **GPOS kerning** — some modern fonts (especially Google Fonts) store kerning only in the GPOS
  table, not the legacy `kern` table. These fonts will have correct advances but no kerning
  adjustment at the base tier.
- **Ligatures** — fi, fl, ffi ligatures and other GSUB substitutions will not apply.
- **Contextual forms** — script-specific glyph substitutions.
- **Mark positioning** — combining diacritics may not be positioned correctly.

For Latin text with fonts that include a `kern` table (which includes the embedded Public Sans
fallback and most traditional fonts), the base tier produces good-quality output. The quality gap
is most visible with Google Fonts and other web-first fonts that rely solely on GPOS.

### Known limitation: stb_truetype double-kerning bug

stb_truetype has a known bug where fonts containing both `kern` and `GPOS` tables get
double-applied kerning from `stbtt_GetGlyphKernAdvance()`. Since the base tier does not process
GPOS, this manifests as slightly over-kerned text with some fonts. The `text_full` tier
(HarfBuzz) eliminates this issue entirely by handling all positioning through GPOS.

### Bazel feature flags {#bazel-flags}

Donner exposes a module extension that lets downstream consumers declare their desired
configuration in `MODULE.bazel`. The extension generates a `@donner_config` repo whose
`config.bzl` supplies the defaults for donner's `bool_flag`/`string_flag` targets. Flags remain
individually overridable on the command line.

#### Downstream consumer usage {#downstream-config}

```starlark
# Downstream project's MODULE.bazel
bazel_dep(name = "donner", version = "1.0")

donner = use_extension("@donner//config:extensions.bzl", "donner")
donner.configure(
    renderer = "tiny_skia",
    text = True,
    text_full = False,
)
use_repo(donner, "donner_config")
```

The `configure` tag validates values at module-resolution time (before the build starts):
- `renderer`: `"skia"` (default) or `"tiny_skia"`
- `text`: `True`/`False` (default `True`)
- `text_full`: `True`/`False` (default `False`)

If no `donner.configure()` call is made (or donner is the root module), all options use their
built-in defaults.

Consumers can still override individual flags on the command line:

```sh
# Override one flag without changing the MODULE.bazel configuration:
bazel build --@donner//donner/svg/renderer:text=false //...
```

#### How flags consume the config

Donner's `BUILD.bazel` loads the generated defaults and uses them as `build_setting_default`
values. The `config_setting` targets and all `select()` references remain local — only the
defaults change:

```python
# donner/svg/renderer/BUILD.bazel
load("@donner_config//:config.bzl", "DONNER_CONFIG")

string_flag(
    name = "renderer_backend",
    build_setting_default = DONNER_CONFIG.get("renderer", "skia"),
    visibility = ["//visibility:public"],
)

bool_flag(
    name = "text",
    build_setting_default = DONNER_CONFIG.get("text", True),
    visibility = ["//visibility:public"],
)

bool_flag(
    name = "text_full",
    build_setting_default = DONNER_CONFIG.get("text_full", False),
    visibility = ["//visibility:public"],
)

config_setting(
    name = "text_enabled",
    flag_values = {":text": "true"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "text_full_enabled",
    flag_values = {":text_full": "true"},
    visibility = ["//visibility:public"],
)

# ... existing renderer_backend_skia / renderer_backend_tiny_skia config_settings unchanged
```

This design has several properties:

- **All `select()` references stay local** — no BUILD files need to change to reference
  `@donner_config` in `select()` keys. Only the flag *defaults* come from the generated repo.
- **Command-line overrides still work** — `--@donner//donner/svg/renderer:text=false` overrides
  the consumer-configured default, because the flags are still standard `bool_flag`/`string_flag`
  targets.
- **`.bazelrc` shortcuts still work** — donner's own `.bazelrc` shortcuts compose with the
  module extension defaults.
- **Validation happens early** — `attr.string(values=["skia", "tiny_skia"])` rejects invalid
  values at module resolution time, before the build starts.
- **Backwards compatible** — if a downstream consumer does `bazel_dep(name = "donner")` without
  calling the extension, donner's own `donner.configure()` with defaults takes effect.

#### Migration from existing flags

The existing `use_coretext` and `use_fontconfig` bool_flags in
`donner/svg/renderer/BUILD.bazel` migrate into the same pattern — their defaults come from
`DONNER_CONFIG` instead of being hardcoded. The `renderer_backend` string_flag already exists
and gains the same treatment. This unifies all donner build-time configuration under a single
`donner.configure()` call.

#### `.bazelrc` shortcuts

Donner's `.bazelrc` still provides shortcuts for internal development:

```sh
# .bazelrc
common:text --//donner/svg/renderer:text=true
common:text-full --//donner/svg/renderer:text=true --//donner/svg/renderer:text_full=true
common:skia --//donner/svg/renderer:renderer_backend=skia
common:tiny-skia --//donner/svg/renderer:renderer_backend=tiny_skia
```

These override the module extension defaults for the donner repo itself and are not inherited
by downstream consumers.

### How features gate code {#feature-gating}

The feature flags control dependencies and defines via `select()`:

```python
# donner/svg/renderer/BUILD.bazel

donner_cc_library(
    name = "text_layout",
    srcs = ["TextLayout.cc"],
    hdrs = ["TextLayout.h"],
    deps = [
        "//donner/svg/resources:font_manager",
    ],
)

donner_cc_library(
    name = "renderer_driver",
    # ...
    deps = [
        ":renderer_interface",
        # ...
    ] + select({
        ":text_enabled": [":text_layout"],
        "//conditions:default": [],
    }),
    defines = select({
        ":text_enabled": ["DONNER_TEXT_ENABLED"],
        "//conditions:default": [],
    }) + select({
        ":text_full_enabled": ["DONNER_TEXT_FULL_ENABLED"],
        "//conditions:default": [],
    }),
)
```

In C++, the driver and backends use `#ifdef DONNER_TEXT_ENABLED` to guard text codepaths. When
text is disabled, `drawText()` remains a no-op stub. When text is enabled but WOFF2 is not,
`FontManager` logs a warning on WOFF2 font encounters and skips them.

```cpp
// FontManager.cc
std::optional<FontHandle> FontManager::loadFontData(std::span<const uint8_t> data) {
  const uint32_t magic = readBE32(data);
  if (magic == 0x774F4646) {       // WOFF 1.0
    return loadWoff1(data);
  } else if (magic == 0x774F4632) { // WOFF 2.0
#ifdef DONNER_TEXT_WOFF2_ENABLED
    return loadWoff2(data);
#else
    UTILS_LOG(warning) << "WOFF2 font encountered but WOFF2 support not enabled. "
                          "Build with --config=text-full to enable.";
    return std::nullopt;
#endif
  }
  // Raw TTF/OTF
  return loadRawFont(data);
}
```

### Test infrastructure

Tests declare required features via the existing `ImageComparisonParams` mechanism:

```cpp
// renderer_tests
{"text_basic.svg", Params::RequiresFeature(Feature::Text)},
{"text_woff2_font.svg", Params::RequiresFeature(Feature::TextFull)},
```

Tests requiring `Feature::Text` are skipped when `DONNER_TEXT_ENABLED` is not defined. This
mirrors the existing `Feature::FilterEffects` pattern.

## Current State

### What works

| Component | Status |
|-----------|--------|
| `<text>`, `<tspan>` parsing | Done — attributes, content nodes, positioning lists |
| `ComputedTextComponent` | Done — resolved spans with x/y/dx/dy/rotate |
| `TextParams` | Done — font families, size, fill/stroke colors, opacity, text-anchor, dominant-baseline, text-decoration, textLength |
| WOFF 1.0 parsing | Done — `WoffParser` decompresses tables via zlib |
| WOFF 2.0 parsing | Done — `Woff2Parser` via Google woff2 + Brotli (`--config=text-full`) |
| `@font-face` CSS parsing | Done — `FontFace` struct with local/url/data sources |
| `FontLoader` | Done — loads WOFF from URI/data, returns `FontResource` |
| `FontManager` | Done — TTF/OTF/WOFF1 loading, `@font-face` cascade, fallback to Public Sans |
| `TextLayout` | Done — stb_truetype layout with kern-table kerning, text-anchor, dominant-baseline, textLength |
| `TextShaper` | Done — HarfBuzz shaping with GSUB/GPOS (`--config=text-full`) |
| Skia `drawText` | Done — fill, stroke, rotation, text-anchor, dominant-baseline, text-decoration |
| TinySkia `drawText` | Done — glyph outlines via stb_truetype/HarfBuzz, fill, stroke, rotation |
| Embedded fallback font | Done — Public Sans Medium OTF in `third_party/public-sans` |

### What's remaining

| Feature | Skia | TinySkia |
|---------|------|----------|
| `<textPath>` rendering | No | No |
| Bidirectional text (RTL) | No | No |
| Vertical writing modes | No | No |
| Variable fonts | No | No |
| System font discovery | Partial (via SkFontMgr) | No (embedded/loaded only) |
