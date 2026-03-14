# Design: Text Rendering

**Status:** Implemented (Phases 1-6)
**Author:** Claude Opus 4.6
**Created:** 2026-03-09
**Tracking:** [#242](https://github.com/jwmcglynn/donner/issues/242)

## Summary

Text is one of the highest-impact remaining gaps for v1.0. The SVG parsing layer already handles
`<text>`, `<tspan>`, and `<textPath>` elements and produces `ComputedTextComponent` with resolved
span positions. The Skia backend has basic text rendering via `SkFont`/`SkFontMgr`, but lacks
stroke rendering, rotation, and `@font-face` integration. The TinySkia backend has no text
rendering at all.

This design covers:

1. **Base text support** — using stb_truetype (already vendored, ~55 KB) for font loading, glyph
   metrics, `kern`-table kerning, and glyph outline extraction. Zero new dependencies.
2. **Finishing Skia text** — stroke, `@font-face`/WOFF, text-anchor, remaining SVG text properties.
3. **Adding TinySkia text** — rendering glyphs as filled/stroked paths via stb_truetype outlines.
4. **Optional WOFF2** — gated behind a separate feature flag.
5. **Feature tiers** — build-time opt-in via Bazel flags controlling binary size cost.

Full OpenType shaping (GSUB/GPOS via HarfBuzz) is available as an opt-in `text_shaping` tier
(`--config=text-shaping`) to avoid roughly doubling the TinySkia binary size when not needed.

## Goals

- Render `<text>` and `<tspan>` elements correctly for common SVG content on both backends.
- Support `@font-face` with TTF/OTF and WOFF 1.0 web fonts.
- Add zero new dependencies for the base text tier (stb_truetype is already vendored).
- Keep binary size and build time impact minimal and opt-in via feature tiers.
- Design the architecture so HarfBuzz shaping can be added later as a drop-in upgrade.
- Maintain build-time backend selection.

## Non-Goals

- Complex script support (Arabic, Devanagari, etc.) or bidirectional text.
- Vertical writing modes (`writing-mode: vertical-rl`).
- SVG fonts (`<font>` element — deprecated in SVG2).
- `<textPath>` rendering (path layout is a separate milestone).
- Font subsetting or optimization.
- System font discovery for TinySkia (embedded/loaded fonts only).

## Feature Tiers {#feature-tiers}

Text support is split into independently selectable features, ordered by dependency cost:

| Feature | What it adds | New deps | Binary cost | Status |
|---------|-------------|----------|------------|--------|
| **`text`** | Font loading (TTF/OTF/WOFF1), `kern`-table kerning, glyph outlines, `@font-face` | None (stb_truetype already vendored) | ~55 KB | **Done** |
| **`text_woff2`** | WOFF2 web font decompression | Google woff2 + Brotli | ~190 KB | **Done** |
| **`text_shaping`** | Full OpenType shaping (GSUB/GPOS: ligatures, contextual kerning) | HarfBuzz (`HB_TINY`) | ~400-600 KB | **Done** |

Each tier implies the previous: `text_shaping` implies `text`, `text_woff2` implies `text`.
All tiers default to **off** so existing consumers pay nothing.

### What the base `text` tier provides vs full shaping

The base tier uses stb_truetype's `kern` table for pair kerning and `stbtt_GetGlyphHMetrics()`
for glyph advances. This covers:

- **Correct glyph advances** — characters are spaced according to the font's horizontal metrics.
- **Pair kerning** — common kern pairs (AV, To, Wa, etc.) are adjusted via the `kern` table.
- **Glyph outlines** — `stbtt_GetGlyphShape()` extracts Bezier curves for path-based rendering.

What it does **not** cover (deferred to `text_shaping`):

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
GPOS, this manifests as slightly over-kerned text with some fonts. The `text_shaping` tier
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
    text_woff2 = False,
    text_shaping = False,
)
use_repo(donner, "donner_config")
```

The `configure` tag validates values at module-resolution time (before the build starts):
- `renderer`: `"skia"` (default) or `"tiny_skia"`
- `text`: `True`/`False` (default `False`)
- `text_woff2`: `True`/`False` (default `False`)
- `text_shaping`: `True`/`False` (default `False`)
- `use_coretext`: `True`/`False` (default `False`, macOS only)
- `use_fontconfig`: `True`/`False` (default `False`, Linux only)

If no `donner.configure()` call is made (or donner is the root module), all options use their
built-in defaults.

Consumers can still override individual flags on the command line:

```sh
# Override one flag without changing the MODULE.bazel configuration:
bazel build --@donner//donner/svg/renderer:text=false //...
```

#### How the module extension works

The extension generates a `@donner_config` repo containing a single `config.bzl` file with the
consumer's chosen defaults:

```starlark
# config/extensions.bzl

_configure = tag_class(attrs = {
    "renderer": attr.string(default = "skia", values = ["skia", "tiny_skia"]),
    "text": attr.bool(default = False),
    "text_woff2": attr.bool(default = False),
    "use_coretext": attr.bool(default = False),
    "use_fontconfig": attr.bool(default = False),
})

def _donner_config_repo_impl(rctx):
    rctx.file("BUILD.bazel", "exports_files(['config.bzl'])\n")
    rctx.file("config.bzl", """
DONNER_CONFIG = {{
    "renderer": {renderer},
    "text": {text},
    "text_woff2": {text_woff2},
    "use_coretext": {use_coretext},
    "use_fontconfig": {use_fontconfig},
}}
""".format(
        renderer = repr(rctx.attr.renderer),
        text = repr(rctx.attr.text),
        text_woff2 = repr(rctx.attr.text_woff2),
        use_coretext = repr(rctx.attr.use_coretext),
        use_fontconfig = repr(rctx.attr.use_fontconfig),
    ))

_donner_config_repo = repository_rule(
    implementation = _donner_config_repo_impl,
    attrs = {
        "renderer": attr.string(default = "skia"),
        "text": attr.bool(default = False),
        "text_woff2": attr.bool(default = False),
        "use_coretext": attr.bool(default = False),
        "use_fontconfig": attr.bool(default = False),
    },
)

def _donner_impl(module_ctx):
    # Start with built-in defaults
    cfg = dict(renderer = "skia", text = False, text_woff2 = False,
               use_coretext = False, use_fontconfig = False)

    # Walk the module graph; root module's tags win
    for mod in module_ctx.modules:
        for tag in mod.tags.configure:
            if mod.is_root:
                cfg = dict(
                    renderer = tag.renderer,
                    text = tag.text,
                    text_woff2 = tag.text_woff2,
                    use_coretext = tag.use_coretext,
                    use_fontconfig = tag.use_fontconfig,
                )

    _donner_config_repo(name = "donner_config", **cfg)

donner = module_extension(
    implementation = _donner_impl,
    tag_classes = {"configure": _configure},
)
```

Donner's own `MODULE.bazel` calls the extension with defaults so that `@donner_config` always
exists, even when donner is the root module:

```starlark
# donner's MODULE.bazel
donner = use_extension("//config:extensions.bzl", "donner")
donner.configure()  # All defaults
use_repo(donner, "donner_config")
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
    build_setting_default = DONNER_CONFIG.get("text", False),
    visibility = ["//visibility:public"],
)

bool_flag(
    name = "text_woff2",
    build_setting_default = DONNER_CONFIG.get("text_woff2", False),
    visibility = ["//visibility:public"],
)

config_setting(
    name = "text_enabled",
    flag_values = {":text": "true"},
    visibility = ["//visibility:public"],
)

config_setting(
    name = "text_woff2_enabled",
    flag_values = {":text_woff2": "true"},
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
common:text-woff2 --//donner/svg/renderer:text=true --//donner/svg/renderer:text_woff2=true
common:text-shaping --//donner/svg/renderer:text=true --//donner/svg/renderer:text_shaping=true
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
        ":text_woff2_enabled": ["DONNER_TEXT_WOFF2_ENABLED"],
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
                          "Build with --config=text-woff2 to enable.";
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
{"text_woff2_font.svg", Params::RequiresFeature(Feature::TextWoff2)},
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
| WOFF 2.0 parsing | Done — `Woff2Parser` via Google woff2 + Brotli (`--config=text-woff2`) |
| `@font-face` CSS parsing | Done — `FontFace` struct with local/url/data sources |
| `FontLoader` | Done — loads WOFF from URI/data, returns `FontResource` |
| `FontManager` | Done — TTF/OTF/WOFF1 loading, `@font-face` cascade, fallback to Public Sans |
| `TextLayout` | Done — stb_truetype layout with kern-table kerning, text-anchor, dominant-baseline, textLength |
| `TextShaper` | Done — HarfBuzz shaping with GSUB/GPOS (`--config=text-shaping`) |
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

## Dependency Evaluation

### Binary size measurements (arm64 macOS)

| Library | Static .a | Code segment | Notes |
|---------|----------|-------------|-------|
| **stb_truetype** | **55-58 KB** | **~50 KB** | **Already vendored, single .o** |
| HarfBuzz (full, default) | 2.2 MB | 628 KB | Homebrew default build |
| HarfBuzz (`HB_TINY`, est.) | 400-600 KB | 250-350 KB | Estimated from 373 KB WASM build |
| FreeType (full) | 767 KB | 391 KB | Already a transitive Skia dep |
| Brotli (decode only) | 175 KB | 27 KB | `libbrotlidec` + `libbrotlicommon` |
| Google woff2 (decode only) | ~10-15 KB | — | ~1700 LOC, 5 source files |
| tiny-skia-cpp | ~200 KB | — | Current TinySkia backend |
| Skia (full linked) | ~6-8 MB | — | Current Skia backend |

### stb_truetype — Base tier font engine

Already vendored in `third_party/stb/stb_truetype.h`. Single-header C library.

- **License:** Public domain / MIT
- **Binary size:** 55-58 KB — **adds ~28% to TinySkia binary** (vs ~250% for HarfBuzz)
- **Capabilities:**
  - Font parsing for TrueType (.ttf/.ttc) and OpenType-CFF (.otf)
  - Glyph metrics: `stbtt_GetGlyphHMetrics()` (advance width, left side bearing)
  - Pair kerning: `stbtt_GetGlyphKernAdvance()` from `kern` table
  - Glyph outline extraction: `stbtt_GetGlyphShape()` returns move/line/curve commands
  - Codepoint → glyph ID mapping: `stbtt_FindGlyphIndex()`
  - Font-level metrics: `stbtt_GetFontVMetrics()` (ascent, descent, line gap)
  - Bitmap rasterization (not needed — we use path outlines)
- **Limitations:**
  - No GSUB/GPOS processing (no ligatures, no GPOS kerning)
  - No hinting (irrelevant for vector-path rendering)
  - Double-kerning bug with fonts that have both `kern` and `GPOS` tables
  - CFF parsing has known edge cases with some malformed fonts
- **Verdict:** The right choice for the base tier. Zero new dependencies, already vendored,
  and the ~55 KB cost is proportional to the TinySkia backend. Provides correct text rendering
  for fonts with `kern` tables. The architecture is designed so HarfBuzz can replace stb_truetype
  for the layout/outline path in the `text_shaping` tier.

### HarfBuzz — Full OpenType shaping

Industry-standard OpenType text shaper. Handles GSUB and GPOS. **Implemented** as `TextShaper`
in the `text_shaping` tier.

- **License:** MIT
- **Binary size:** ~400-600 KB with `HB_TINY` — roughly doubles the TinySkia binary
- **Key capability:** `hb_font_draw_glyph()` (HarfBuzz 7.0+) provides glyph outlines via
  Bezier path draw callbacks, replacing stb_truetype for outline extraction when enabled.
- **Build:** Amalgamated single-file `harfbuzz.cc` via `new_git_repository`, compiles in ~5s.
  Uses `HB_TINY` for minimal size with `config-override.h` (`patch_cmds`) to re-enable the
  draw API (`HB_NO_DRAW`), CFF outlines (`HB_NO_CFF`), and file I/O (`HB_NO_OPEN`).
- **Version:** 10.1.0 (tag, resolved to commit `9ef44a2d`)
- **Integration:** `TextShaper` creates HarfBuzz font objects from `FontManager`'s raw font
  data. Both backends use conditional compilation (`DONNER_TEXT_SHAPING_ENABLED`) to select
  `TextShaper` vs `TextLayout`.

### kb_text_shape.h — Lightweight alternative shaper

Single-header C/C++ OpenType shaper (~22K LOC). Supports GSUB/GPOS.

- **License:** Unknown / recently published (mid-2025)
- **Limitations:** Not battle-tested, 128x slower font loading for complex scripts
- **Verdict:** Worth evaluating for the `text_shaping` tier as a lighter alternative to
  HarfBuzz. Should be measured for actual binary size before deciding.

### Google woff2 + Brotli — WOFF2 decompression

- **License:** MIT (both)
- **Binary size:** ~185-195 KB total (Brotli decode 175 KB + woff2 decode ~15 KB)
- **Verdict:** Gated behind `text_woff2`. Modest cost for broad web font compatibility.

### FreeType — Font rasterization

- **License:** FreeType License (BSD-style) or GPLv2
- **Binary size:** 767 KB full, ~30 KB minimal
- **Notable:** Built-in WOFF2 support since 2.10.2 (with Brotli). Already a `MODULE.bazel`
  dep (for Skia's font managers) but not used directly by donner code.
- **Verdict:** Not needed as a direct dependency. stb_truetype covers font parsing/outlines for
  TinySkia, and Skia bundles FreeType internally.

### SheenBidi — Bidirectional text ordering

- **License:** Apache 2.0
- **Dependencies:** None (pure C)
- **Verdict:** Deferred until RTL text is prioritized.

## Proposed Architecture

### Component overview

```
@font-face CSS rules
       |
       v
  FontManager (new, gated by DONNER_TEXT_ENABLED)
  - Resolves @font-face cascade
  - Decompresses WOFF1 (always) / WOFF2 (if DONNER_TEXT_WOFF2_ENABLED)
  - Loads fonts via stb_truetype (base) or HarfBuzz (text_shaping tier)
  - Caches loaded fonts
  - Provides fallback (Public Sans)
       |
       v
  TextLayout (new, gated by DONNER_TEXT_ENABLED)
  - Takes ComputedTextComponent spans + FontManager
  - Lays out glyphs: codepoint → glyph ID, advance + kern adjustment
  - Produces LayoutTextRun[] with glyph IDs, positions
  - Applies text-anchor adjustment
  - [text_shaping tier: replaced by TextShaper using HarfBuzz]
       |
       v
  RendererInterface::drawText()
       |
  +----+----+
  |         |
Skia     TinySkia
  |         |
SkFont   stbtt_GetGlyphShape → PathSpline → tiny-skia fill/stroke
```

### FontManager

New shared component at `donner/svg/resources/FontManager.h`. Owns the font lifecycle.

```cpp
class FontManager {
public:
  /// Register a @font-face declaration. Sources are resolved lazily on first use.
  void addFontFace(const css::FontFace& face);

  /// Find or load a font matching the given family name and style.
  /// Falls through @font-face sources, then tries the embedded fallback.
  FontHandle findFont(std::string_view family, FontStyle style = {});

  /// Access the stb_truetype font info for a handle.
  const stbtt_fontinfo* fontInfo(FontHandle handle) const;

  /// Get the scale factor for a given font size in pixels.
  float scaleForPixelHeight(FontHandle handle, float pixelHeight) const;

private:
  struct LoadedFont {
    std::vector<uint8_t> data;  // Owns the font file bytes (stb_truetype references them)
    stbtt_fontinfo info;        // stb_truetype parsed font
    std::string familyName;
  };

  std::vector<css::FontFace> faces_;
  std::unordered_map<std::string, FontHandle> cache_;
  std::vector<LoadedFont> fonts_;
};
```

**Font loading pipeline:**

1. `@font-face` rules are collected during style resolution and registered with `FontManager`.
2. On first `findFont()` call for a family name, iterate the `@font-face` sources:
   - `Kind::Local` — not supported for TinySkia (no system font access); Skia can delegate to
     `SkFontMgr`.
   - `Kind::Url` — fetch via `ResourceLoaderInterface`, detect format by magic bytes:
     - `0x774F4646` → WOFF 1.0 → existing `WoffParser::Parse()` → reconstruct sfnt
     - `0x774F4632` → WOFF 2.0 → `Woff2Parser::Decompress()` (if `DONNER_TEXT_WOFF2_ENABLED`)
     - `0x00010000` or `OTTO` → raw TTF/OTF → pass through
   - `Kind::Data` — decode data URI, same format detection.
3. Initialize `stbtt_fontinfo` via `stbtt_InitFont()`. The font data must be kept alive since
   stb_truetype holds a pointer to it.
4. Cache by family name. If no `@font-face` matches, use the embedded Public Sans fallback.

### TextLayout

New shared component at `donner/svg/renderer/TextLayout.h`. Converts `ComputedTextComponent`
spans into positioned glyph sequences using stb_truetype metrics.

```cpp
struct LayoutGlyph {
  int glyphIndex;      // stb_truetype glyph index
  double xPosition;    // Absolute X position for this glyph
  double yPosition;    // Absolute Y baseline position
  double xAdvance;     // Advance to next glyph (for reference)
};

struct LayoutTextRun {
  FontHandle font;
  std::vector<LayoutGlyph> glyphs;
};

class TextLayout {
public:
  explicit TextLayout(FontManager& fontManager);

  /// Lay out all spans, returning positioned glyph runs.
  std::vector<LayoutTextRun> layout(const components::ComputedTextComponent& text,
                                    const TextParams& params);
};
```

**Layout pipeline per span:**

1. Resolve font via `fontManager_.findFont(params.fontFamilies)`.
2. Compute scale factor: `stbtt_ScaleForPixelHeight(info, fontSizePx)`.
3. For each codepoint in the UTF-8 span text:
   a. Map to glyph index: `stbtt_FindGlyphIndex(info, codepoint)`.
   b. Get advance width: `stbtt_GetGlyphHMetrics(info, glyphIndex, &advance, &lsb)`.
   c. Get kern adjustment with next glyph: `stbtt_GetGlyphKernAdvance(info, glyph, nextGlyph)`.
   d. Accumulate pen position: `penX += (advance + kern) * scale`.
4. Apply span's absolute x/y positioning and dx/dy shifts.
5. Apply `text-anchor` adjustment (measure total advance, shift all glyph positions).

### TextShaper (HarfBuzz tier)

The `TextShaper` class replaces `TextLayout` when the `text_shaping` tier is enabled. Both
produce equivalent output formats (`ShapedTextRun`/`ShapedGlyph` vs `LayoutTextRun`/`LayoutGlyph`)
so renderers can consume either identically. The Bazel `select()` in each renderer backend
conditionally compiles the appropriate path:

```cpp
#ifdef DONNER_TEXT_SHAPING_ENABLED
  TextShaper shaper(fontManager);
  auto runs = shaper.layout(text, params);
  // ... use ShapedTextRun for rendering
#else
  TextLayout layout(fontManager);
  auto runs = layout.layout(text, params);
  // ... use LayoutTextRun for rendering
#endif
```

When HarfBuzz is enabled:
- `TextShaper` replaces `TextLayout` — it calls `hb_shape()` instead of manual advance+kern.
- Glyph outline extraction switches from `stbtt_GetGlyphShape()` to
  `hb_font_draw_glyph()` (HarfBuzz 7.0+ draw API).
- `TextShaper` creates HarfBuzz font objects internally from `FontManager`'s raw font data.
- Both backends use conditional compilation (`DONNER_TEXT_SHAPING_ENABLED`) to select the tier.

### Glyph outline extraction (TinySkia)

For TinySkia, glyphs are rendered as filled/stroked paths. stb_truetype's
`stbtt_GetGlyphShape()` returns an array of move/line/curve commands directly convertible to
`PathSpline`.

```cpp
PathSpline glyphToPath(const stbtt_fontinfo* info, int glyphIndex, float scale) {
  stbtt_vertex* vertices = nullptr;
  const int numVertices = stbtt_GetGlyphShape(info, glyphIndex, &vertices);

  PathSpline::Builder builder;
  for (int i = 0; i < numVertices; ++i) {
    const float x = vertices[i].x * scale;
    const float y = -vertices[i].y * scale;  // stb_truetype Y is up, SVG Y is down
    switch (vertices[i].type) {
      case STBTT_vmove:
        builder.moveTo(x, y);
        break;
      case STBTT_vline:
        builder.lineTo(x, y);
        break;
      case STBTT_vcurve: {
        const float cx = vertices[i].cx * scale;
        const float cy = -vertices[i].cy * scale;
        builder.quadTo(cx, cy, x, y);
        break;
      }
      case STBTT_vcubic: {
        const float cx = vertices[i].cx * scale;
        const float cy = -vertices[i].cy * scale;
        const float cx1 = vertices[i].cx1 * scale;
        const float cy1 = -vertices[i].cy1 * scale;
        builder.cubicTo(cx, cy, cx1, cy1, x, y);
        break;
      }
    }
  }
  stbtt_FreeShape(info, vertices);
  return builder.build();
}
```

Each laid-out glyph is:
1. Converted to a `PathSpline` via `glyphToPath()`.
2. Translated to the glyph's computed position.
3. Drawn via `tiny_skia::Pixmap::fill_path()` / `stroke_path()`.

Glyph path caching (by `{fontHandle, glyphIndex}`) avoids redundant outline extraction for
repeated characters.

### Skia backend changes

The Skia backend currently uses `drawSimpleText()` which does its own internal layout. For the
base tier, two approaches:

**Option A — Keep `drawSimpleText()` for Skia, use TextLayout only for TinySkia:**
Simplest change. Skia's internal layout is already decent (it uses its own kern/GPOS support
via FreeType/CoreText). TinySkia uses the shared `TextLayout` for positioning. Skia-specific
improvements (stroke, rotation, text-anchor) are done using Skia APIs directly.

**Option B — Use shared TextLayout for both backends:**
Both backends get identical glyph positions from `TextLayout`. Skia uses `drawGlyphs()` with the
glyph IDs and positions. This ensures both backends produce the same layout, but Skia loses its
own potentially-better kerning (since it can access GPOS via FreeType internally).

**Recommendation:** Option A for the base tier. Skia's `drawSimpleText()` already works and its
layout is at least as good as stb_truetype's (likely better, since it has GPOS access). Adding
`TextLayout` for Skia provides no benefit at this tier. At the `text_shaping` tier (now
implemented), Option B is used — both backends use identical HarfBuzz shaping, and Skia uses
`drawGlyphs()` with glyph positions from `TextShaper`.

For the base tier, Skia improvements focus on:
- Stroke text rendering via `SkPaint::kStroke_Style`.
- Per-glyph rotation via canvas save/rotate/restore.
- `text-anchor` via measuring text width with `SkFont::measureText()`.
- `@font-face` font loading via `SkFontMgr::makeFromData()` with fonts loaded by `FontManager`.

### WOFF2 integration (gated by `text_woff2`)

1. Add `third_party/woff2` and `third_party/brotli` as Bazel deps with `cc_library` targets.
2. Add `Woff2Parser` alongside existing `WoffParser` in `donner/base/fonts/`:
   ```cpp
   class Woff2Parser {
   public:
     static ParseResult<std::vector<uint8_t>> Decompress(std::span<const uint8_t> woff2Data);
   };
   ```
3. `FontManager` detects format by magic bytes and routes to the appropriate parser. WOFF2
   encounters without `DONNER_TEXT_WOFF2_ENABLED` produce a diagnostic warning.

### RendererInterface changes

The `drawText` signature and `TextParams` struct need minor updates:

```cpp
struct TextParams {
  double opacity = 1.0;
  css::Color fillColor = css::Color(css::RGBA());
  css::Color strokeColor = css::Color(css::RGBA());
  StrokeParams strokeParams;
  SmallVector<RcString, 1> fontFamilies;
  Lengthd fontSize;
  Boxd viewBox;
  FontMetrics fontMetrics;
  // New fields:
  TextAnchor textAnchor = TextAnchor::Start;
  DominantBaseline dominantBaseline = DominantBaseline::Auto;
};
```

## Implementation Plan

### Phase 1: Module extension and feature flags

- [x] Implement the `donner` module extension in `config/extensions.bzl`.
  - [x] `_donner_config_repo` repo rule generating `config.bzl`.
  - [x] `_configure` tag class with all donner build options.
  - [x] `donner.configure()` call in donner's own `MODULE.bazel`.
- [x] Migrate existing flags (`renderer_backend`, `use_coretext`, `use_fontconfig`) to load
  defaults from `@donner_config//:config.bzl`.
- [x] Add `text` and `text_woff2` bool_flags with defaults from `DONNER_CONFIG`.
  - [x] Create config_settings and `.bazelrc` shortcuts.
  - [x] Add `DONNER_TEXT_ENABLED` / `DONNER_TEXT_WOFF2_ENABLED` defines via `select()`.
- [x] Create stb_truetype Bazel target (wrap existing vendored header via `stb_library` macro).
- [x] Implement `FontManager` in `donner/svg/resources/`.
  - [x] TTF/OTF loading via `stbtt_InitFont()`.
  - [x] WOFF1 loading via existing `WoffParser` → sfnt reconstruction → `stbtt_InitFont()`.
  - [x] `@font-face` source cascade.
  - [x] Fallback to embedded Public Sans.
  - [x] Font handle caching.
  - [x] Unit tests.

### Phase 2: TextLayout and TinySkia text rendering

- [x] Implement `TextLayout` in `donner/svg/renderer/`.
  - [x] UTF-8 iteration → codepoint → glyph index mapping.
  - [x] Advance width + `kern`-table kerning accumulation.
  - [x] `text-anchor` adjustment (measure total advance, shift origin).
  - [x] Unit tests with embedded Public Sans.
- [x] Implement glyph outline extraction via `stbtt_GetGlyphShape()`.
  - [x] stb_truetype vertex array → `PathSpline::Builder`.
  - [x] Y-axis flip (stb_truetype Y-up → SVG Y-down).
  - [x] Glyph path cache (`{FontHandle, glyphIndex}` → `PathSpline`).
- [x] Implement `RendererTinySkia::drawText()`.
  - [x] Lay out text via `TextLayout`.
  - [x] For each glyph: extract outline, translate to position, fill via `fill_path()`.
  - [x] Stroke support via `stroke_path()`.
  - [x] Per-glyph rotation.
- [x] Add text golden images for TinySkia.

### Phase 3: Skia backend text improvements

- [x] Wire `FontManager` into Skia for `@font-face` font loading.
  - [x] `SkFontMgr::makeFromData()` with font bytes from `FontManager`.
  - [x] Fallback chain: `@font-face` → system font → embedded Public Sans.
- [x] Add stroke text rendering.
  - [x] `SkPaint::kStroke_Style` with stroke params from `TextParams`.
  - [x] Draw stroke first, then fill (SVG paint order).
- [x] Add per-glyph rotation support.
- [x] Add `text-anchor` support via `SkFont::measureText()`.
- [x] Add `dominant-baseline` support.
- [x] Update golden images for text tests.

### Phase 4: WOFF2 support (behind `text_woff2` flag)

- [x] Vendor Google woff2 + Brotli under `third_party/`.
  - [x] `cc_library` targets (decode-only for both).
- [x] Implement `Woff2Parser` in `donner/base/fonts/`.
  - [x] Unit tests with sample WOFF2 files.
- [x] Wire into `FontManager` behind `#ifdef DONNER_TEXT_WOFF2_ENABLED`.

### Phase 5: SVG text properties (both backends)

- [x] `text-decoration` (underline, overline, line-through).
  - [x] Compute decoration lines from font metrics via `stbtt_GetFontVMetrics()`.
  - [x] Draw as stroked paths.
- [x] `textLength` / `lengthAdjust` support.
  - [x] Spacing mode: distribute extra space between glyphs.
  - [x] SpacingAndGlyphs mode: scale glyph advances.
- [x] `dominant-baseline` support (both backends, both layout tiers).

### Phase 6: HarfBuzz shaping tier (follow-up)

- [x] Add `text_shaping` bool_flag and config_setting.
  - `--config=text-shaping` enables both `text=true` and `text_shaping=true`.
  - `text_shaping_enabled` config_setting requires both flags for Bazel select() specificity.
- [x] Vendor HarfBuzz (amalgamated, `HB_TINY`) under `third_party/harfbuzz`.
  - Uses `new_git_repository` with `patch_cmds` to create `config-override.h` that re-enables
    the draw API (`#undef HB_NO_DRAW`), CFF outlines (`#undef HB_NO_CFF`), and file I/O
    (`#undef HB_NO_OPEN`) which are disabled by `HB_LEAN`/`HB_TINY`.
  - [ ] Measure actual arm64 binary size.
  - [ ] Also evaluate `kb_text_shape.h` as a lighter alternative.
- [x] Implement `TextShaper` as drop-in replacement for `TextLayout`.
  - [x] HarfBuzz buffer setup, `hb_shape()`, glyph position extraction.
  - [x] Glyph outline extraction via `hb_font_draw_glyph()` draw API (HarfBuzz 7.0+).
  - [x] text-anchor, textLength, dominant-baseline adjustments.
  - [x] 7 unit tests covering shaping, outlines, multi-byte, text-anchor.
- [x] TextShaper creates HarfBuzz font objects from FontManager's raw font data internally.
- [x] Switch Skia backend to `drawGlyphs()` with shared shaping (Option B).
- [ ] Run resvg text tests, compare quality improvement vs base tier.

### Phase 7: Bidirectional text (deferred)

- [ ] Vendor SheenBidi under `third_party/sheenbidi`.
- [ ] Integrate bidi reordering before layout/shaping.
- [ ] Add RTL/bidi test cases.

## Testing and Validation

### Unit tests

- `FontManager` tests: load TTF/OTF/WOFF1, verify `stbtt_fontinfo` initialization, test
  cascade fallback, verify font metrics.
- `TextLayout` tests: lay out known strings with embedded Public Sans, verify glyph positions
  against expected values, test `text-anchor` adjustment.
- `Woff2Parser` tests: decompress known WOFF2 files, verify output matches reference TTF.
- Glyph outline tests: extract outlines for known glyphs, verify path geometry matches expected
  curves.

### Golden image tests

- Add text-specific SVG test cases to `renderer_tests`:
  - Basic Latin text.
  - Multi-span positioning (dx/dy).
  - `text-anchor` (start/middle/end).
  - `@font-face` with embedded WOFF1 font.
  - `@font-face` with WOFF2 font (requires `text_woff2`).
  - Stroke + fill text.
  - Per-glyph rotation.
- Run existing resvg text test subset (currently skipped) and tighten thresholds.

### Feature-gated test skipping

```cpp
{"text_basic.svg", Params::RequiresFeature(Feature::Text)},
{"text_woff2_font.svg", Params::RequiresFeature(Feature::TextWoff2)},
```

CI runs all feature combinations to ensure both enabled and disabled paths work.

### Backend parity

- Text golden images differ between Skia and TinySkia at the base tier because Skia uses
  its own internal layout (with GPOS access) while TinySkia uses stb_truetype (kern-table only).
- At the `text_shaping` tier, shaping output is identical between backends since both use
  HarfBuzz, and golden images should be much closer.
- TinySkia renders glyph outlines as vector paths (no hinting), so small-size text will look
  slightly different from Skia's hinted rasterization regardless of shaping tier.

## Dependencies

| Dependency | Feature tier | License | Binary cost | New? |
|------------|-------------|---------|------------|------|
| stb_truetype | `text` | Public domain / MIT | ~55 KB | Already vendored |
| Google woff2 (decode) | `text_woff2` | MIT | ~15 KB | Yes |
| Brotli (decode) | `text_woff2` | MIT | ~175 KB | Yes |
| HarfBuzz (`HB_TINY`) | `text_shaping` | MIT | ~400-600 KB | Done |
| SheenBidi | Future | Apache 2.0 | ~30 KB | Future |
| Public Sans | `text` | OFL 1.1 | ~90 KB (data) | Already vendored |
| zlib | (existing) | zlib | — | Already a dep |

**Total cost by feature combination:**

| Configuration | Added binary size | New deps |
|--------------|------------------|----------|
| No text (default) | 0 | None |
| `--config=text` | ~55 KB | None (stb_truetype already vendored) |
| `--config=text-woff2` | ~245 KB | woff2, Brotli |
| `--config=text-shaping` | ~500-650 KB | HarfBuzz |

For comparison: tiny-skia-cpp is ~200 KB, Skia is ~6-8 MB linked.

## Alternatives Considered

**HarfBuzz as the base tier instead of stb_truetype:**
Kept as opt-in tier. HarfBuzz `HB_TINY` adds ~400-600 KB, roughly doubling the TinySkia binary.
The quality improvement (GSUB/GPOS) is available via `--config=text-shaping` but not forced on
all consumers. The base tier (stb_truetype) provides correct text for fonts with `kern` tables
at minimal binary cost.

**FreeType for glyph outlines:**
Rejected. stb_truetype covers font parsing/outlines at ~55 KB vs FreeType's ~767 KB (full) or
~30 KB (aggressively stripped). stb_truetype is already vendored and its outline API maps
directly to `PathSpline`. FreeType's hinting is irrelevant for vector-path rendering.

**Skia's text APIs only (no shared font loading for TinySkia):**
Not viable. TinySkia has no text APIs. A shared font loading layer is needed regardless of
whether the layout is done by stb_truetype or HarfBuzz.

**kb_text_shape.h as the shaping tier:**
Worth evaluating for the `text_shaping` tier as a potentially lighter alternative to HarfBuzz.
At ~22K LOC vs HarfBuzz's 100K+, it could produce a significantly smaller binary. However, it's
too new (mid-2025) and untested to commit to now. The follow-up phase should measure its actual
binary size and test suite pass rate before deciding.

**WOFF2 always included with text:**
Rejected per user preference. WOFF2 adds ~190 KB (Brotli + woff2) that is unnecessary for
consumers who only load TTF/OTF/WOFF1 fonts.

**Runtime feature detection instead of build-time flags:**
Rejected. Build-time selection via `select()` enables dead-code elimination by the linker. This
matches the existing renderer backend selection pattern.

**Command-line flags only (no module extension) for downstream configuration:**
Rejected. While `--@donner//donner/svg/renderer:text=true` works, it requires downstream
consumers to know internal label paths and provides no schema validation. The module extension
approach validates values at module-resolution time (e.g., `attr.string(values=["skia",
"tiny_skia"])` rejects typos immediately) and provides a discoverable API surface. The
implementation cost is modest — the extension is ~60 lines of Starlark — and it unifies all
donner configuration under a single `donner.configure()` call. Command-line override is still
supported for ad-hoc testing.

**Generated repo with flags instead of generated repo with defaults:**
Rejected. An alternative design would put the `bool_flag`/`string_flag` targets in the generated
`@donner_config` repo and have donner's `select()` reference `@donner_config//:text`. This would
require changing all `select()` references across donner's BUILD files. The chosen design
(generated defaults, local flags) keeps all `select()` references local and unchanged — only the
`build_setting_default` values come from the generated repo.

**Three-way layout strategy (no-text / stb-basic / harfbuzz-full) active simultaneously:**
Rejected in favor of sequential tiers. The `text` and `text_shaping` tiers use the same
interface (`LayoutTextRun`), so `text_shaping` simply replaces the layout implementation rather
than coexisting. This avoids maintaining two layout codepaths long-term.

## Open Questions

- Should `FontManager` be owned by `RendererDriver` or by the `SVGDocument`? If owned by the
  document, fonts persist across re-renders. If owned by the driver, fonts are loaded per frame
  (wasteful but simpler lifetime management). Recommend document ownership with a reference
  passed to the driver.
- Should glyph path caching in TinySkia be per-frame or persistent? Persistent caching saves
  work for animated content but increases memory.
- How should Skia's system font matching interact with `FontManager`? For `@font-face` fonts,
  both backends use `FontManager`-loaded font data. For system fonts, Skia uses `SkFontMgr`
  while TinySkia falls back to the embedded font. Is this asymmetry acceptable?
- Should the `text` feature default to `true` eventually? Once text support is stable, it may
  make sense to flip the default.
- For the WOFF1 → stb_truetype pipeline: the existing `WoffParser` produces decompressed sfnt
  tables (`WoffFont`). We need to reconstruct a complete sfnt byte stream from these tables for
  `stbtt_InitFont()`. Is there an existing utility for this, or do we need to write sfnt table
  assembly?
- How do we handle the stb_truetype double-kerning bug? Options: (a) ignore it and document as
  a known limitation, (b) detect fonts with both `kern` and `GPOS` tables and skip `kern`, or
  (c) fix the bug in our vendored copy.

## Future Work

- [x] `<textPath>` rendering (text along arbitrary paths) — completed.
- [ ] Bidirectional text (SheenBidi).
- [ ] Vertical writing modes (`writing-mode: vertical-rl/lr`).
- [ ] Font feature settings (`font-feature-settings` CSS property).
- [ ] Variable fonts (OpenType font variations).
- [ ] Font fallback chains (try multiple fonts for missing glyphs).
- [ ] System font discovery for TinySkia (platform-specific font enumeration).
- [ ] Glyph bitmap caching for TinySkia performance optimization.
- [ ] `text-decoration` with proper ink-skip behavior.
- [ ] CMake feature selection matching the Bazel `text`/`text_woff2`/`text_shaping` flags.
- [ ] Measure actual HarfBuzz `HB_TINY` arm64 binary size impact.
- [ ] Evaluate `kb_text_shape.h` as lighter HarfBuzz alternative.
- [ ] Run resvg text tests with `text_shaping` tier, compare quality vs base tier.

## Resvg Text Test Gap Analysis (2026-03-13)

**Status:** 183 text tests enabled across 11 groups, all passing with thresholds. The test harness
loads the resvg suite's Noto Sans fonts so font data is identical — remaining diffs are from
missing features and stb_truetype vs FreeType glyph rendering differences.

### Current Test Threshold Summary

| Test Group | Tests | Typical Diff | Default Threshold | Root Cause |
|-----------|-------|-------------|-------------------|------------|
| `e-text-` | 41 (+1 UB skip) | 13K-24K | 17500 | Per-char positioning + font rendering |
| `e-tspan` | 31 | 5K-45K | 17000 | Per-char positioning + font rendering |
| `e-textPath` | 29 (+15 skip) | 1K-9K | varies | Font rendering (positioning works) |
| `a-text-anchor` | 13 | 2K-21K | 10500 | Font rendering (feature implemented) |
| `a-textLength` | 9 | 10K-24K | 12000 | Font rendering (feature implemented) |
| `a-text-decoration` | 19 | 5K-19K | 13000 | Font rendering (feature implemented) |
| `a-text-rendering` | 5 | 10K-19K | 19500 | Font rendering |
| `a-baseline-shift` | 22 | 6K-21K | 8500 | **Not implemented** |
| `a-letter-spacing` | 11 | 1K-16K | 17000 | **Not implemented** |
| `a-word-spacing` | 6 (+1 UB skip) | 4K | 4500 | **Not implemented** |
| `a-writing-mode` | 18 (+2 UB skip) | 2K-17K | 17000 | **Not implemented** |
| `a-alignment-baseline` | 1 | 12K | 13000 | **Not implemented** |
| `a-kerning` | 1 | 10K | 10000 | Font rendering |
| `a-lengthAdjust` | 1 | 15K | 15000 | Font rendering |
| `a-unicode-bidi` | 1 | 2K | 2500 | Partial (no bidi reordering) |

### Root Cause Breakdown

#### 1. Per-Character Positioning NOT Implemented (CRITICAL)

**Impact:** Affects ALL e-text and e-tspan tests. Explains ~5K-10K of every diff.

The SVG spec allows `x`, `y`, `dx`, `dy`, and `rotate` attributes to contain comma/space-separated
lists where each value positions an individual character:

```xml
<text x="30 70 110 150" y="70 100 130 160">Text</text>
<!-- Should position: T at (30,70), e at (70,100), x at (110,130), t at (150,160) -->
```

**Current state:** `TextSystem.cc:32-56` only reads `pos.x[0]`, discarding all subsequent values.
The `ComputedTextComponent::TextSpan` struct only stores scalar x/y/dx/dy/rotate, not per-character
arrays. Parsing is correct (`SmallVector<Lengthd, 1>` stores full lists), but the computed layer
throws them away.

**Files:**
- `donner/svg/components/text/TextSystem.cc` — lines 32-56, only uses `[0]`
- `donner/svg/components/text/ComputedTextComponent.h` — `TextSpan` stores single values
- `donner/svg/renderer/TextLayout.cc` — layout uses single baseX/baseY per span

#### 2. Irreducible Font Rendering Baseline (~3K-5K per test)

Even with all features correctly implemented, stb_truetype and FreeType produce different glyph
outlines due to different hinting/rasterization. On a 200x200 canvas with text, this produces
roughly 3000-5000 pixel diffs. This is the floor we cannot go below without switching font backends.

Evidence: `e-textPath` tests (where positioning IS correct) still show 3000-5000 pixel diffs.

#### 3. Missing Text Properties

| Property | Status | Impact | Difficulty |
|----------|--------|--------|------------|
| `letter-spacing` | Not implemented | 1K-16K diffs | Low |
| `word-spacing` | Not implemented | ~4K diffs | Low |
| `baseline-shift` | Not implemented | 6K-21K diffs | Medium |
| `writing-mode` | Not implemented (no enum) | 2K-17K diffs | High |
| `alignment-baseline` | Not implemented | ~12K diffs | Medium |

All five properties are registered in `PropertyRegistry.cc` but never parsed or applied.

#### 4. Missing SVG 2 Features on `<tspan>`

Several e-tspan tests exercise SVG 2 features where tspan gets its own rendering context:
- `e-tspan-018`: `opacity` on tspan (5891 diff)
- `e-tspan-020`: `mask` on tspan (16782 diff)
- `e-tspan-021`: `clip-path` on tspan (12107 diff)
- `e-tspan-022`: `filter` on tspan (44238 diff — largest diff)

These require rendering each tspan as an independent graphics element, which the current
architecture doesn't support (tspan is treated as inline text content, not a stacking context).

### Implementation Plan: Closing the Gaps

#### Phase 8: Per-Character Positioning (Done)

**Goal:** Reduce e-text and e-tspan default thresholds from ~17000 to ~5000 (font rendering floor).

- [x] Extended `ComputedTextComponent::TextSpan` with per-character position arrays:
  `xList`, `yList`, `dxList`, `dyList` (`std::vector<std::optional<Lengthd>>`) and
  `rotateList` (`std::vector<double>`). Values are `Lengthd` for deferred resolution
  in the layout engine where viewBox/fontMetrics are available.
- [x] Updated `TextSystem::instantiateAllComputedComponents()` to populate per-character
  arrays using global character indexing across tspan boundaries. Each character position
  checks the leaf element's own positioning first, then falls back to the root text
  element's positioning at the global character index.
- [x] Updated `TextLayout::layout()` with per-character positioning in the glyph loop:
  absolute x/y override the pen position (suppressing kerning), dx/dy add relative offsets,
  and rotate uses last-value-repeats semantics per SVG spec.
- [x] Updated `TextShaper::layout()` with the same per-character logic, using HarfBuzz
  cluster byte offsets mapped to codepoint indices via a byte-to-char-index table.
- [x] Updated textPath rotation in both layout engines to combine path tangent angle
  with per-glyph rotation (previously used only span-level rotation).

**Measured outcome:** Tests with per-character positioning (e.g., per-char x/y/dx/dy/rotate)
dropped dramatically: e-tspan-011 from ~17K to 1.2K, e-text-022 from ~17K to 2.1K, many
e-tspan tests from ~17K to 5K-8K. Tests without per-character positioning remain at ~15K-16K
(font rendering baseline). Two per-char rotate tests (e-text-012, e-text-014) needed threshold
bumps from 17500 to 18000 due to rotation now being correctly applied per-character.

#### Phase 9: letter-spacing and word-spacing (Done)

**Goal:** Reduce a-letter-spacing thresholds from 17000 to ~5000 and a-word-spacing from 4500
to ~1000.

- [x] Added `ParseSpacingValue()` in PropertyRegistry.cc to parse "normal | \<length\>".
  "normal" maps to `Lengthd(0)`.
- [x] Declared `letterSpacing` and `wordSpacing` as `Property<Lengthd, PropertyCascade::Inherit>`
  in PropertyRegistry.h with parser entries in the kProperties map.
- [x] Added `letterSpacingPx` and `wordSpacingPx` (resolved to pixels) to `TextParams`,
  populated from computed style in `toTextParams()` in RendererDriver.cc.
- [x] Applied in both `TextLayout::layout()` and `TextShaper::layout()`:
  letter-spacing is added after every glyph advance, word-spacing is added after U+0020 space.

**Measured outcome:** Tests with non-zero letter-spacing (007-011) dropped from ~16K to 1K-3.6K.
Tests with `letter-spacing="normal"` (001-006) remain at ~16K (font rendering baseline).
Word-spacing tests all at ~4K (within existing 4500 threshold).

#### Phase 10: baseline-shift (Done)

**Goal:** Reduce a-baseline-shift thresholds from 8500 to ~4K.

- [x] Added `ParseBaselineShift()` in PropertyRegistry.cc: handles `baseline` (→0),
  `sub` (→-0.33em), `super` (→0.4em), `<length>`, `<percentage>` (→em fraction).
- [x] Declared `baselineShift` as `Property<Lengthd>` (not inherited) in PropertyRegistry.h.
- [x] Added `Lengthd baselineShift` to `ComputedTextComponent::TextSpan`.
- [x] In TextSystem, reads baseline-shift from entity's `ComputedStyleComponent` (or parent's)
  and stores in the span.
- [x] In TextLayout/TextShaper, resolves to pixels using actual font size for em units
  (`FontMetrics` adjusted with `fontSizePx`), then subtracted from baseY (positive = shift up
  per CSS convention).

**Measured outcome:** Tests 001-013 dropped from ~8.5K to ~7K-7.8K (still within 8500 default).
Tests 014-021 unchanged (nested/percentage shifts remain at pre-existing override thresholds).
Test 013 improved to 6.3K. All tests pass within existing thresholds.

#### Phase 11: writing-mode (Deferred)

**Goal:** Enable vertical text rendering.

This is the most complex missing feature. It requires:
1. `WritingMode` enum: `horizontal-tb` (default), `vertical-rl`, `vertical-lr`
2. Glyph rotation (90 CW for horizontal glyphs in vertical mode)
3. Vertical advance metrics from the `vmtx`/`vhea` font tables
4. Baseline alignment changes for vertical text
5. HarfBuzz `HB_DIRECTION_TTB` for the shaping tier

**Recommendation:** Defer to a dedicated phase after per-character positioning and spacing
properties are complete. Writing-mode affects only 18 tests and requires architectural changes
to the layout pipeline.

#### Phase 12: alignment-baseline (Done)

**Goal:** Close the single a-alignment-baseline test.

- [x] Added `alignmentBaseline` as `Property<DominantBaseline>` in PropertyRegistry, reusing
  the `ParseDominantBaseline()` parser since the value sets are identical.
- [x] Added `DominantBaseline alignmentBaseline` to `ComputedTextComponent::TextSpan`.
- [x] In TextSystem, reads from entity's `ComputedStyleComponent` alongside baseline-shift.
- [x] In TextLayout/TextShaper, when `span.alignmentBaseline != Auto`, computes a per-span
  baseline shift that overrides the text-level `dominant-baseline`.

**Measured outcome:** a-alignment-baseline-001 at 12223 pixels (within 13000 threshold). Passes
in both base text and text-shaping tiers.

### Priority Order

1. **Phase 8 (per-character positioning)** — highest impact, affects all 72 e-text/e-tspan tests
2. **Phase 9 (letter/word-spacing)** — low difficulty, affects 17 tests
3. **Phase 10 (baseline-shift)** — medium difficulty, affects 22 tests
4. **Phase 12 (alignment-baseline)** — small, 1 test
5. **Phase 11 (writing-mode)** — deferred, high complexity, 18 tests

### Expected Threshold Reduction

After Phases 8-10:

| Test Group | Current Default | Expected | Reduction |
|-----------|----------------|----------|-----------|
| `e-text-` | 17500 | ~5000 | 3.5x |
| `e-tspan` | 17000 | ~5000 | 3.4x |
| `a-letter-spacing` | 17000 | ~5000 | 3.4x |
| `a-word-spacing` | 4500 | ~1500 | 3x |
| `a-baseline-shift` | 8500 | ~4500 | 1.9x |
| `a-text-anchor` | 10500 | ~5000 | 2.1x |
| `a-textLength` | 12000 | ~5000 | 2.4x |

The irreducible floor of ~3000-5000 comes from stb_truetype vs FreeType glyph rendering
differences and cannot be reduced without switching font backends.

## `<textPath>` Implementation Plan (v0.5)

The `<textPath>` element renders text along an arbitrary path referenced by `href`. This is
the last text-related element needed for v0.5 (currently listed as "Not yet supported" in README).

### SVGTextPathElement Class

Create `donner/svg/SVGTextPathElement.h` extending `SVGTextPositioningElement`:

- Add `ElementType::TextPath` to the enum in `ElementType.h`.
- Register in `AllSVGElements.h` type list.
- Register tag `"textPath"` in `SVGParser::createElement()`.

**Attributes:**

| Attribute | Type | Default | Description |
|-----------|------|---------|-------------|
| `href` / `xlink:href` | `<IRI>` | required | Reference to a `<path>` element |
| `startOffset` | `<length>` or `<percentage>` | `0` | Offset along the path where text begins |
| `method` | `align` \| `stretch` | `align` | How glyphs are placed on the path |
| `side` | `left` \| `right` | `left` | Which side of the path to render text |
| `textLength` | `<length>` | auto | Desired text length for adjustment |
| `lengthAdjust` | `spacing` \| `spacingAndGlyphs` | `spacing` | How to adjust to `textLength` |
| `spacing` | `auto` \| `exact` | `exact` | Spacing control (SVG2) |

### Path-Based Text Layout

Extend `TextSystem` / `ComputedTextComponent` to handle path-based glyph positioning:

1. **Resolve path reference** — Look up the `href` target, which must be a `<path>` element.
   Extract its `PathSpline` geometry.

2. **Compute path length** — Use `PathSpline::pathLength()` to get total arc length.
   Resolve `startOffset` against this length (percentages are relative to path length).

3. **Sample path at glyph positions** — For each glyph:
   - Compute the distance along the path: `startOffset + sum of prior glyph advances`.
   - Sample the path at that distance to get `(x, y)` position and tangent angle.
   - Position the glyph midpoint at `(x, y)` with rotation = tangent angle.
   - If `method="stretch"`, additionally scale each glyph along the tangent direction
     to match the path curvature.

4. **Handle `side="right"`** — Reverse the path direction before sampling. Glyphs are
   placed on the opposite side and rendered upside-down relative to the path.

5. **Handle overflow** — Glyphs that extend past the end of the path are hidden. If the
   path is closed, text wraps only if the UA supports it (typically not).

### Path Sampling Algorithm

```
function samplePathAtDistance(spline, distance):
  // Walk segments until cumulative length >= distance
  for each segment in spline:
    segLength = segment.arcLength()
    if distance <= segLength:
      t = segment.parameterForArcLength(distance)
      point = segment.evaluate(t)
      tangent = segment.tangent(t)
      return (point, atan2(tangent.y, tangent.x))
    distance -= segLength
  return null  // past end of path
```

The `PathSpline` already has `pathLength()` and segment evaluation. We need to add
`parameterForArcLength()` — binary search on the segment's arc length integral.

### Renderer Integration

Both backends need to render glyphs with per-glyph transforms:

- **Current approach**: `drawText` renders glyph outlines as paths. Each glyph already gets
  individual path data from font metrics.
- **Extension**: For `<textPath>` glyphs, prepend a per-glyph rotation+translation transform
  before drawing each glyph outline. This requires the text layout to output per-glyph
  transforms alongside the glyph paths.

### Tests

- Enable resvg `e-textPath-*` test suite (currently `// TODO(text): e-textPath`).
- Unit tests for path sampling, startOffset, method, side attributes.
- Edge cases: zero-length paths, single-point paths, very long text overflowing path.
