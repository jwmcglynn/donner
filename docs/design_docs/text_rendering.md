# Design: Text Rendering

**Status:** Design
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

Full OpenType shaping (GSUB/GPOS via HarfBuzz) is deferred as a follow-up `text_shaping` tier
to avoid roughly doubling the TinySkia binary size in the initial implementation.

## Goals

- Render `<text>` and `<tspan>` elements correctly for common SVG content on both backends.
- Support `@font-face` with TTF/OTF and WOFF 1.0 web fonts.
- Add zero new dependencies for the base text tier (stb_truetype is already vendored).
- Keep binary size and build time impact minimal and opt-in via feature tiers.
- Design the architecture so HarfBuzz shaping can be added later as a drop-in upgrade.
- Maintain build-time backend selection.

## Non-Goals

- Full OpenType shaping (GSUB/GPOS — ligatures, contextual forms) in the initial implementation.
  Deferred to the `text_shaping` tier (HarfBuzz follow-up).
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
| **`text`** | Font loading (TTF/OTF/WOFF1), `kern`-table kerning, glyph outlines, `@font-face` | None (stb_truetype already vendored) | ~55 KB | This design |
| **`text_woff2`** | WOFF2 web font decompression | Google woff2 + Brotli | ~190 KB | This design |
| **`text_shaping`** | Full OpenType shaping (GSUB/GPOS: ligatures, contextual kerning) | HarfBuzz (`HB_TINY`) | ~400-600 KB | Follow-up |

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
)
use_repo(donner, "donner_config")
```

The `configure` tag validates values at module-resolution time (before the build starts):
- `renderer`: `"skia"` (default) or `"tiny_skia"`
- `text`: `True`/`False` (default `False`)
- `text_woff2`: `True`/`False` (default `False`)
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
| `TextParams` | Done — font families, size, fill/stroke colors, opacity |
| WOFF 1.0 parsing | Done — `WoffParser` decompresses tables via zlib |
| `@font-face` CSS parsing | Done — `FontFace` struct with local/url/data sources |
| `FontLoader` | Done — loads WOFF from URI/data, returns `FontResource` |
| Skia `drawText` | Partial — fills text, resolves font family via `SkFontMgr` |
| Embedded fallback font | Done — Public Sans Medium OTF in `third_party/public-sans` |
| stb_truetype | Vendored in `third_party/stb/` — not yet used for text |

### What's missing

| Feature | Skia | TinySkia |
|---------|------|----------|
| Text rendering | Partial (fill only) | None (stub) |
| `kern`-table kerning | No (Skia does its own) | No |
| Stroke rendering | No | No |
| Per-glyph rotation | No | No |
| `text-anchor` | No | No |
| `dominant-baseline` | No | No |
| `text-decoration` | No | No |
| `@font-face` integration | No (uses SkFontMgr) | No |
| WOFF2 decompression | No | No |
| OpenType shaping (GSUB/GPOS) | No | No |
| Bidirectional text | No | No |

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

### HarfBuzz — Full OpenType shaping (follow-up tier)

Industry-standard OpenType text shaper. Handles GSUB and GPOS.

- **License:** MIT
- **Binary size:** ~400-600 KB with `HB_TINY` — roughly doubles the TinySkia binary
- **Key capability:** `hb_font_get_glyph_shape()` (HarfBuzz 7.0+) provides glyph outlines via
  Bezier path callbacks, replacing stb_truetype for outline extraction when enabled.
- **Build:** Amalgamated single-file `harfbuzz.cc`, compiles in <15s
- **Verdict:** Deferred to `text_shaping` follow-up tier. The binary size cost is significant
  relative to the TinySkia backend. When added, HarfBuzz replaces stb_truetype for both layout
  and outline extraction, eliminating the double-kerning issue and adding full GSUB/GPOS support.

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

### Designing for the HarfBuzz upgrade path

The `TextLayout` class is designed to be replaceable by a `TextShaper` that uses HarfBuzz in the
`text_shaping` tier. Both produce the same `LayoutTextRun`/`LayoutGlyph` output — the rendering
code in both backends consumes this interface identically. The Bazel `select()` swaps the
implementation:

```python
# Future text_shaping tier
donner_cc_library(
    name = "text_layout_impl",
    deps = select({
        ":text_shaping_enabled": [":text_shaper"],  # HarfBuzz-based
        "//conditions:default": [":text_layout"],     # stb_truetype-based
    }),
)
```

When HarfBuzz is enabled:
- `TextShaper` replaces `TextLayout` — it calls `hb_shape()` instead of manual advance+kern.
- Glyph outline extraction switches from `stbtt_GetGlyphShape()` to
  `hb_font_get_glyph_shape()` (HarfBuzz 7.0+ `hb_draw` API).
- `FontManager` internally uses HarfBuzz font objects instead of `stbtt_fontinfo`.
- The rendering code in both backends is unchanged — it still receives `LayoutTextRun[]`.

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
`TextLayout` for Skia provides no benefit at this tier. When HarfBuzz is added in the
`text_shaping` tier, Option B becomes the right choice — both backends use identical HarfBuzz
shaping, and Skia uses `drawGlyphs()`.

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

- [ ] Implement the `donner` module extension in `config/extensions.bzl`.
  - [ ] `_donner_config_repo` repo rule generating `config.bzl`.
  - [ ] `_configure` tag class with all donner build options.
  - [ ] `donner.configure()` call in donner's own `MODULE.bazel`.
- [ ] Migrate existing flags (`renderer_backend`, `use_coretext`, `use_fontconfig`) to load
  defaults from `@donner_config//:config.bzl`.
- [ ] Add `text` and `text_woff2` bool_flags with defaults from `DONNER_CONFIG`.
  - [ ] Create config_settings and `.bazelrc` shortcuts.
  - [ ] Add `DONNER_TEXT_ENABLED` / `DONNER_TEXT_WOFF2_ENABLED` defines via `select()`.
- [ ] Create stb_truetype Bazel target (wrap existing vendored header via `stb_library` macro).
- [ ] Implement `FontManager` in `donner/svg/resources/`.
  - [ ] TTF/OTF loading via `stbtt_InitFont()`.
  - [ ] WOFF1 loading via existing `WoffParser` → sfnt reconstruction → `stbtt_InitFont()`.
  - [ ] `@font-face` source cascade.
  - [ ] Fallback to embedded Public Sans.
  - [ ] Font handle caching.
  - [ ] Unit tests.

### Phase 2: TextLayout and TinySkia text rendering

- [ ] Implement `TextLayout` in `donner/svg/renderer/`.
  - [ ] UTF-8 iteration → codepoint → glyph index mapping.
  - [ ] Advance width + `kern`-table kerning accumulation.
  - [ ] `text-anchor` adjustment (measure total advance, shift origin).
  - [ ] Unit tests with embedded Public Sans.
- [ ] Implement glyph outline extraction via `stbtt_GetGlyphShape()`.
  - [ ] stb_truetype vertex array → `PathSpline::Builder`.
  - [ ] Y-axis flip (stb_truetype Y-up → SVG Y-down).
  - [ ] Glyph path cache (`{FontHandle, glyphIndex}` → `PathSpline`).
- [ ] Implement `RendererTinySkia::drawText()`.
  - [ ] Lay out text via `TextLayout`.
  - [ ] For each glyph: extract outline, translate to position, fill via `fill_path()`.
  - [ ] Stroke support via `stroke_path()`.
  - [ ] Per-glyph rotation.
- [ ] Add text golden images for TinySkia.

### Phase 3: Skia backend text improvements

- [ ] Wire `FontManager` into Skia for `@font-face` font loading.
  - [ ] `SkFontMgr::makeFromData()` with font bytes from `FontManager`.
  - [ ] Fallback chain: `@font-face` → system font → embedded Public Sans.
- [ ] Add stroke text rendering.
  - [ ] `SkPaint::kStroke_Style` with stroke params from `TextParams`.
  - [ ] Draw stroke first, then fill (SVG paint order).
- [ ] Add per-glyph rotation support.
- [ ] Add `text-anchor` support via `SkFont::measureText()`.
- [ ] Add `dominant-baseline` support.
- [ ] Update golden images for text tests.

### Phase 4: WOFF2 support (behind `text_woff2` flag)

- [ ] Vendor Google woff2 + Brotli under `third_party/`.
  - [ ] `cc_library` targets (decode-only for both).
- [ ] Implement `Woff2Parser` in `donner/base/fonts/`.
  - [ ] Unit tests with sample WOFF2 files.
- [ ] Wire into `FontManager` behind `#ifdef DONNER_TEXT_WOFF2_ENABLED`.

### Phase 5: SVG text properties (both backends)

- [ ] `text-decoration` (underline, overline, line-through).
  - [ ] Compute decoration lines from font metrics via `stbtt_GetFontVMetrics()`.
  - [ ] Draw as stroked paths.
- [ ] `textLength` / `lengthAdjust` support.
  - [ ] Spacing mode: distribute extra space between glyphs.
  - [ ] SpacingAndGlyphs mode: scale glyph advances.

### Phase 6: HarfBuzz shaping tier (follow-up)

- [ ] Add `text_shaping` bool_flag and config_setting.
- [ ] Vendor HarfBuzz (amalgamated, `HB_TINY`) under `third_party/harfbuzz`.
  - [ ] Measure actual arm64 binary size.
  - [ ] Also evaluate `kb_text_shape.h` as a lighter alternative.
- [ ] Implement `TextShaper` as drop-in replacement for `TextLayout`.
  - [ ] HarfBuzz buffer setup, `hb_shape()`, glyph position extraction.
  - [ ] Glyph outline extraction via `hb_font_get_glyph_shape()`.
- [ ] Update `FontManager` to use HarfBuzz font objects when `text_shaping` is enabled.
- [ ] Switch Skia backend to `drawGlyphs()` with shared shaping (Option B).
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

- Text golden images will differ between Skia and TinySkia at the base tier because Skia uses
  its own internal layout (with GPOS access) while TinySkia uses stb_truetype (kern-table only).
- At the `text_shaping` tier (HarfBuzz follow-up), shaping output will be identical between
  backends since both use HarfBuzz, and golden images should be much closer.
- TinySkia renders glyph outlines as vector paths (no hinting), so small-size text will look
  slightly different from Skia's hinted rasterization regardless of shaping tier.

## Dependencies

| Dependency | Feature tier | License | Binary cost | New? |
|------------|-------------|---------|------------|------|
| stb_truetype | `text` | Public domain / MIT | ~55 KB | Already vendored |
| Google woff2 (decode) | `text_woff2` | MIT | ~15 KB | Yes |
| Brotli (decode) | `text_woff2` | MIT | ~175 KB | Yes |
| HarfBuzz (`HB_TINY`) | `text_shaping` (follow-up) | MIT | ~400-600 KB | Future |
| SheenBidi | Future | Apache 2.0 | ~30 KB | Future |
| Public Sans | `text` | OFL 1.1 | ~90 KB (data) | Already vendored |
| zlib | (existing) | zlib | — | Already a dep |

**Total cost by feature combination:**

| Configuration | Added binary size | New deps |
|--------------|------------------|----------|
| No text (default) | 0 | None |
| `--config=text` | ~55 KB | None (stb_truetype already vendored) |
| `--config=text-woff2` | ~245 KB | woff2, Brotli |
| `--config=text-shaping` (future) | ~500-650 KB | HarfBuzz |

For comparison: tiny-skia-cpp is ~200 KB, Skia is ~6-8 MB linked.

## Alternatives Considered

**HarfBuzz as the base tier instead of stb_truetype:**
Deferred. HarfBuzz `HB_TINY` adds ~400-600 KB, roughly doubling the TinySkia binary. The
quality improvement (GSUB/GPOS) is real but not critical for Latin-heavy SVG content with fonts
that include `kern` tables. The architecture is designed so HarfBuzz can be added later as a
drop-in replacement for the layout path, and the `text_shaping` tier is explicitly planned.

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

- [ ] `text_shaping` tier: HarfBuzz or kb_text_shape.h for full GSUB/GPOS.
- [ ] `<textPath>` rendering (text along arbitrary paths).
- [ ] Vertical writing modes (`writing-mode: vertical-rl/lr`).
- [ ] Font feature settings (`font-feature-settings` CSS property).
- [ ] Variable fonts (OpenType font variations).
- [ ] Font fallback chains (try multiple fonts for missing glyphs).
- [ ] System font discovery for TinySkia (platform-specific font enumeration).
- [ ] Glyph bitmap caching for TinySkia performance optimization.
- [ ] `text-decoration` with proper ink-skip behavior.
- [ ] CMake feature selection matching the Bazel `text`/`text_woff2` flags.
- [ ] Bidirectional text (SheenBidi).
