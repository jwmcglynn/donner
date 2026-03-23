# Text Rendering: Architecture

[Back to hub](../text_rendering.md)

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
