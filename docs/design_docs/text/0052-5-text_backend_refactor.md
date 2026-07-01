# Text Backend Refactor: TextBackend Abstraction

[Back to hub](../0010-text_rendering.md)

**Status:** Implemented
**Author:** Claude Opus 4.6
**Created:** 2026-04-02
**Updated:** 2026-04-04

> This refactor is complete. `TextEngine`, `TextBackendSimple`, and `TextBackendFull`
> are the current shipped text architecture.

## Summary

The text stack has two layout engines — `TextLayout` (stb_truetype, "simple") and `TextShaper`
(HarfBuzz + FreeType, "full") — selected at build time via `DONNER_TEXT_FULL`. They share ~600-700
lines of duplicated SVG text layout logic (positioning, chunking, text-anchor, baseline-shift,
textLength) but each bundle their own font-backend calls inline. The renderer (`RendererTinySkia`)
also reaches through `FontManager` to call stb_truetype functions directly (font metrics, post table
parsing), even when the full engine is active.

This refactor introduces a `TextBackend` interface that abstracts all font-backend operations
(metrics, outlines, shaping), then restructures the layout engines to share the backend-agnostic
SVG positioning code through a single `TextEngine`. `FontManager` is now a registry-backed font
service that stores `@font-face` declarations, loaded font bytes, and backend cache components on
font entities inside the same document `entt::registry` used by the rest of the SVG ECS.
`TextEngine` owns the selected backend privately, exposes the narrow text API used by renderers,
layout-adjacent systems, and text DOM public APIs, and is registered in `registry.ctx()` for
shared access. `TextSystem` remains the ECS-facing layer that flattens raw text DOM state into
`ComputedTextComponent`, while `TextEngine` consumes that computed text state to perform shaping,
layout, glyph geometry extraction, and `ComputedTextGeometryComponent` caching for a specific text
root. The two backend implementations are `TextBackendSimple` (stb_truetype) and
`TextBackendFull` (HarfBuzz + FreeType). The refactor also includes a readability pass to reduce
function length and cyclomatic complexity in the engine code.

## Goals

- **No stb_truetype in text-full builds.** When `DONNER_TEXT_FULL` is defined, all font operations
  go through HarfBuzz/FreeType. The renderer must not call `stbtt_*` functions or access
  `stbtt_fontinfo*` directly.
- **Eliminate duplicated layout logic.** Per-character positioning, text-anchor chunking,
  baseline-shift, textLength adjustment, text-on-path — all shared in one place.
- **Clean capability model.** Backend-specific features (cursive detection, bitmap glyphs, OpenType
  feature queries) are explicit in the `TextBackend` API, not hidden behind `#ifdef`.
- **Clear service split.** `FontManager` is a raw font registry only; anything involving shaping,
  metrics, outlines, or multi-glyph behavior goes through `TextEngine`.
- **Rename and unify.** `TextLayout` and `TextShaper` merge into a single `TextEngine` that
  accepts a `TextBackend`. `TextBackendSimple` replaces stb_truetype-specific code,
  `TextBackendFull` replaces HarfBuzz+FreeType code. Output types become `TextGlyph` and `TextRun`.
- **Readability cleanup.** The current `layout()` methods are 700+ lines with deeply nested
  per-span/per-glyph loops. Extract named helpers with clear single responsibilities to reduce
  cyclomatic complexity and make the code easier to review and modify.
- **Testability.** The `TextBackend` interface enables mock-based unit testing of `TextEngine`
  layout logic without real fonts. Each backend gets its own focused unit tests.

## Non-Goals

- Changing the build-time engine selection mechanism (`DONNER_TEXT_FULL` / `--config=text-full`).
- Runtime engine switching.
- Adding new text features (multi-line, `font` shorthand, etc.).
- Adding runtime backend switching.

## Current State

### Problem 1: stb_truetype leaks into the renderer

`RendererTinySkia::drawText()` directly calls:

| stb_truetype call | Purpose | Line |
|---|---|---|
| `fontManager.fontInfo(run.font)` → `stbtt_fontinfo*` | Get font pointer | 1382, 1409 |
| `stbtt_GetFontVMetrics()` | Ascent/descent for em-box bounds | 1419, 1609 |
| `stbtt_ScaleForMappingEmToPixels()` | Em-scale for decoration metrics | 1646 |
| `info->data` / `info->fontstart` | Raw 'post' table parsing for underline metrics | 1620-1643 |

These calls execute even in text-full builds, where HarfBuzz/FreeType could provide the same data.
The `fontInfo()` method returns `nullptr` for bitmap-only fonts, requiring fallback paths.

### Problem 2: ~600 lines of duplicated layout logic

Both engines implement the same SVG text layout algorithm independently:

| Duplicated section | Approx. lines each |
|---|---|
| Dominant-baseline / alignment-baseline shift | 40 |
| Per-span font resolution + size handling | 30 |
| Per-character x/y/dx/dy positioning | 60 |
| Text chunk boundary tracking | 30 |
| Text-anchor adjustment loop | 70 |
| Per-span textLength (spacing/scaling) | 50 |
| Global textLength adjustment | 40 |
| Text-on-path repositioning | 80 |
| Vertical mode branching | 60 |
| Small-caps codepoint conversion | 20 |
| Non-spacing character detection (`isNonSpacing()`) | 30 |
| UTF-8 decoding | 30 |
| OS/2 sub/super offset reading | 30 |
| **Total** | **~570** |

The core difference between the engines is *how glyphs are produced* (codepoint→glyph mapping,
kerning, GSUB/GPOS shaping, advance resolution), not how the resulting glyphs are positioned in SVG
document space.

### Problem 3: Identical output types with different names

```cpp
// TextLayout.h                    // TextShaper.h
struct LayoutGlyph { ... };        struct ShapedGlyph { ... };    // identical fields
struct LayoutTextRun { ... };      struct ShapedTextRun { ... };  // identical fields
```

The renderer has `#ifdef` blocks to handle both, but the actual rendering code is the same.

### Problem 4: High cyclomatic complexity

Both `TextLayout::layout()` (~700 lines) and `TextShaper::layout()` (~1250 lines) are monolithic
functions with deeply nested loops and branches:

- Outer per-span loop contains per-character inner loop
- Vertical/horizontal mode branching duplicates large blocks
- Small-caps, textLength, text-on-path each add conditional layers
- Baseline shift calculations repeated inline at multiple points

This makes the code difficult to review, modify, and test in isolation. Individual concerns
(chunking, anchor adjustment, text-on-path) cannot be unit-tested independently.

## Design

### Layer Diagram

```
┌─────────────────────────────────────────────────────────────┐
│   Renderers / SVGTextContentElement / other text clients    │
│ call TextEngine layout/metrics/outline/cache/public APIs    │
└────────────────────────┬────────────────────────────────────┘
                         │ uses
┌────────────────────────▼────────────────────────────────────┐
│               TextEngine (shared service)                    │
│  layout() → SVG positioning, chunking, anchor, textLength   │
│  cached geometry/public API queries for one text root       │
│  owns selected backend and hides backend choice             │
└────────────────────────┬────────────────────────────────────┘
                         │ owns one of
          ┌──────────────┴──────────────┐
┌─────────▼───────────┐ ┌──────────────▼───────────────────────┐
│ TextBackendSimple    │ │ TextBackendFull                      │
│ stbtt_* calls        │ │ hb_*/FT_* calls                     │
│ glyph outlines       │ │ glyph outlines, bitmaps, cursive    │
│ kern table kerning   │ │ GSUB/GPOS shaping, smcp feature     │
└──────────────────────┘ └─────────────────────────────────────┘
                         │
                         │ consumes
┌────────────────────────▼────────────────────────────────────┐
│                   TextSystem (ECS layer)                     │
│  TextComponent/TextPositioningComponent ->                   │
│  ComputedTextComponent for one text root                    │
└────────────────────────┬────────────────────────────────────┘
                         │ reads raw font bytes / family lookup
┌────────────────────────▼────────────────────────────────────┐
│             FontManager (document ECS-backed)               │
│  @font-face registration, family matching, raw font data,   │
│  and backend cache components stored on font entities       │
└─────────────────────────────────────────────────────────────┘
```

### TextBackend Interface

```cpp
namespace donner::svg::renderer {

/// Font vertical metrics in font design units (unscaled).
struct FontVMetrics {
  int ascent = 0;    ///< Positive, above baseline.
  int descent = 0;   ///< Negative, below baseline.
  int lineGap = 0;
};

/// Underline/strikethrough positioning from the 'post' table.
struct UnderlineMetrics {
  double position = 0;    ///< Distance below baseline (positive = down), in pixels.
  double thickness = 0;   ///< Stroke thickness in pixels.
};

/// Sub/superscript Y offsets from the OS/2 table, in font design units.
struct SubSuperMetrics {
  int subscriptYOffset = 0;
  int superscriptYOffset = 0;
};

/// Result of shaping a single text run (span). Produced by the backend, consumed
/// by TextEngine for SVG positioning.
struct ShapedRun {
  struct ShapedGlyph {
    int glyphIndex = 0;
    double xAdvance = 0;
    double yAdvance = 0;
    float fontSizeScale = 1.0f;  ///< < 1.0 for synthesized small-caps.
  };
  std::vector<ShapedGlyph> glyphs;
};

/// Abstract font backend. Implementations wrap stb_truetype or HarfBuzz+FreeType.
class TextBackend {
public:
  virtual ~TextBackend() = default;

  // ── Font metrics ──────────────────────────────────────────────

  /// Vertical metrics (ascent, descent, lineGap) in font design units.
  virtual FontVMetrics fontVMetrics(FontHandle font) const = 0;

  /// Scale factor: font design units → pixels for the given pixel height.
  virtual float scaleForPixelHeight(FontHandle font, float pixelHeight) const = 0;

  /// Scale factor: em units → pixels (differs from scaleForPixelHeight when
  /// ascent-descent != unitsPerEm).
  virtual float scaleForEmToPixels(FontHandle font, float pixelHeight) const = 0;

  // ── Table-derived metrics ─────────────────────────────────────

  /// Underline position/thickness from the 'post' table, scaled to pixels.
  /// Returns std::nullopt if the table is missing.
  virtual std::optional<UnderlineMetrics> underlineMetrics(FontHandle font,
                                                           float fontSizePx) const = 0;

  /// Sub/superscript Y offsets from the OS/2 table, in font design units.
  /// Returns std::nullopt if the table is missing.
  virtual std::optional<SubSuperMetrics> subSuperMetrics(FontHandle font) const = 0;

  // ── Glyph operations ─────────────────────────────────────────

  /// Extract a glyph outline as a Path. Coordinates are in font units
  /// scaled by `scale`, with Y flipped for SVG's y-down convention.
  virtual Path glyphOutline(FontHandle font, int glyphIndex, float scale) const = 0;

  /// Returns true if the font is bitmap-only (no vector outlines).
  virtual bool isBitmapOnly(FontHandle font) const = 0;

  // ── Shaping ───────────────────────────────────────────────────

  /// Shape a span of text, producing glyph IDs and advances.
  /// The full text and byte range are provided so backends with context-aware
  /// shaping (HarfBuzz) can use surrounding context.
  virtual ShapedRun shapeRun(FontHandle font, float fontSizePx,
                             std::string_view fullText,
                             size_t byteOffset, size_t byteLength,
                             bool isVertical) const = 0;

  /// Compute cross-span kerning adjustment between the last glyph of the
  /// previous span and the first glyph of the current span.
  /// Returns the X (or Y for vertical) kern offset in pixels.
  virtual double crossSpanKern(FontHandle prevFont, float prevSizePx,
                               FontHandle curFont, float curSizePx,
                               uint32_t prevCodepoint, uint32_t curCodepoint,
                               bool isVertical) const = 0;

  // ── Capability queries ────────────────────────────────────────

  /// Returns true if letter-spacing should be suppressed for this codepoint
  /// (cursive/connected scripts like Arabic). Simple backends return false.
  virtual bool isCursive(uint32_t codepoint) const = 0;

  /// Returns true if the font has a native OpenType small-caps feature (smcp).
  /// If false, the engine synthesizes small-caps via uppercase + reduced size.
  virtual bool hasSmallCapsFeature(FontHandle font) const = 0;

  // ── Bitmap glyphs (optional) ──────────────────────────────────

  struct BitmapGlyph {
    std::vector<uint8_t> rgbaPixels;
    int width = 0;
    int height = 0;
    double bearingX = 0;
    double bearingY = 0;
    double scale = 1.0;
  };

  /// Extract a bitmap glyph (CBDT/CBLC color emoji). Returns std::nullopt if
  /// the glyph is not a bitmap. Simple backends always return std::nullopt.
  virtual std::optional<BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex,
                                                  float scale) const = 0;
};

}  // namespace donner::svg::renderer
```

### Unified Output Types

Replace `LayoutGlyph`/`ShapedGlyph` and `LayoutTextRun`/`ShapedTextRun` with `TextGlyph`/`TextRun`:

```cpp
namespace donner::svg::renderer {

/// A positioned glyph in a laid-out text run.
struct TextGlyph {
  int glyphIndex = 0;
  double xPosition = 0;
  double yPosition = 0;
  double xAdvance = 0;
  double yAdvance = 0;
  double rotateDegrees = 0;
  float fontSizeScale = 1.0f;
};

/// A run of positioned glyphs sharing the same font.
struct TextRun {
  FontHandle font;
  std::vector<TextGlyph> glyphs;
};

}  // namespace donner::svg::renderer
```

### TextEngine (Shared Text Service)

```cpp
class TextEngine {
public:
  TextEngine(FontManager& fontManager, Registry& registry);

  /// Full SVG text layout: shaping → positioning → chunking → anchor → textLength.
  std::vector<TextRun> layout(const components::ComputedTextComponent& text,
                              const TextParams& params);

  FontVMetrics fontVMetrics(FontHandle font) const;
  Path glyphOutline(FontHandle font, int glyphIndex, float scale) const;
  std::optional<UnderlineMetrics> underlineMetrics(FontHandle font) const;
  std::optional<TextBackend::BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex,
                                                      float scale) const;
  std::optional<double> measureChUnitInEm(std::span<const RcString> fontFamilies);
  void prepareForElement(EntityHandle handle, std::vector<ParseError>* outWarnings = nullptr);

private:
  FontManager& fontManager_;
  Registry& registry_;
  std::unique_ptr<TextBackend> backend_;

  // ── Shared helpers (currently duplicated) ───────────────────
  static double calculateBaselineShift(DominantBaseline baseline,
                                       const FontVMetrics& metrics, float scale);
  static double calculateAlignmentShift(AlignmentBaseline alignment,
                                        const FontVMetrics& metrics, float scale);
  void applyTextAnchor(std::vector<TextRun>& runs,
                       std::span<const ChunkBoundary> chunks);
  void applyTextLength(std::vector<TextRun>& runs, /* ... */);
  void applyTextOnPath(std::vector<TextRun>& runs, /* ... */);
};
```

The `layout()` method contains all the shared SVG positioning code. It calls the internally-owned
backend to produce glyph IDs and advances, then positions them according to SVG text layout rules
(per-character coordinates, dominant-baseline, text-anchor, textLength, etc.). Callers never
observe the backend type directly.

### TextBackendSimple and TextBackendFull

```cpp
/// stb_truetype-based backend. No GSUB/GPOS, no bitmap glyphs, no cursive detection.
class TextBackendSimple final : public TextBackend {
public:
  TextBackendSimple(FontManager& fontManager, Registry& registry);

  // All methods implemented via stbtt_* calls.
  // isCursive() → always false
  // hasSmallCapsFeature() → always false
  // bitmapGlyph() → always std::nullopt
  // shapeRun() → stbtt_FindGlyphIndex + stbtt_GetGlyphHMetrics + stbtt_GetGlyphKernAdvance
};

/// HarfBuzz + FreeType backend. Full OpenType shaping, cursive detection, bitmap glyphs.
class TextBackendFull final : public TextBackend {
public:
  TextBackendFull(FontManager& fontManager, Registry& registry);

  // All methods implemented via hb_*/FT_* calls.
  // isCursive() → Unicode range checks for Arabic, Syriac, Thaana, N'Ko, Mandaic
  // hasSmallCapsFeature() → hb_ot_layout_language_find_feature(HB_TAG('s','m','c','p'))
  // bitmapGlyph() → FT_Load_Glyph with FT_LOAD_COLOR
  // shapeRun() → hb_buffer_add_utf8 + hb_shape
};
```

### Renderer Changes

`RendererTinySkia::drawText()` and the former full-Skia renderer's `drawText()` stop instantiating backends directly.
The render driver passes the active document `Registry&` into `drawText()`, and the renderers use
shared `FontManager` / `TextEngine` instances already stored in `registry.ctx()`. `RendererDriver`
still resolves renderer-facing paint state, while layout-facing per-span state is delegated to
`TextEngine`.

```cpp
void RendererTinySkia::drawText(Registry& registry, const ComputedTextComponent& text,
                                const TextParams& params) {
  auto& textEngine = registry.ctx().get<TextEngine>();
  std::vector<TextRun> runs = textEngine.layout(text, toTextLayoutParams(params));

  // --- Font metrics for text bbox: use engine, not stbtt ---
  for (const auto& run : runs) {
    FontVMetrics metrics = textEngine.fontVMetrics(run.font);
    float scale = textEngine.scaleForPixelHeight(run.font, fontSizePx);
    double emTop = static_cast<double>(metrics.ascent) * scale;
    double emBottom = -static_cast<double>(metrics.descent) * scale;
    // ... bbox computation ...
  }

  // --- Glyph outlines: use engine, not stbtt/shaper ---
  for (const auto& glyph : run.glyphs) {
    Path path = textEngine.glyphOutline(run.font, glyph.glyphIndex, scale);
    // ... no #ifdef needed ...
  }

  // --- Text decoration: use engine, not raw table parsing ---
  auto underline = textEngine.underlineMetrics(run.font);
  // ... position decoration lines ...
}
```

### Test Changes

`ImageComparisonTestFixture` currently parses font tables manually (`fontFamilyFromData`,
`fontWeightFromData`, `fontStyleFromData`). These should either:

1. Move into `FontManager` as proper API (since they operate on raw font data before a FontHandle
   exists), or
2. Use a standalone table-reading utility shared with the backends.

The `FontManager_tests.cc` tests that validate stb_truetype behavior directly should remain as
unit tests for `TextBackendSimple`.

## Migration Plan

### Phase 1: TextBackend interface + TextBackendSimple

- [x] Define `TextBackend` interface in `donner/svg/renderer/TextBackend.h`
- [x] Define unified `TextGlyph` and `TextRun` types (`TextTypes.h`)
- [x] Implement `TextBackendSimple` wrapping stb_truetype calls
- [x] Refactor `RendererTinySkia` to use `TextBackend` instead of `stbtt_fontinfo*`
- [x] All existing tests pass with no behavioral changes
- [x] Add unit tests for `TextBackendSimple` (metrics, outlines, shaping)

### Phase 2: TextEngine extraction

- [x] Extract layout algorithm from `TextLayout::layout()` into `TextEngine::layout()`
- [x] Implement `TextBackendSimple::shapeRun()` and `crossSpanKern()`
- [x] `RendererTinySkia` uses `TextEngine` for the simple (non-text-full) path
- [x] Chunk-based shaping: engine splits spans at absolute positions before calling `shapeRun()`
- [x] All renderer_tests and resvg_test_suite pass (base config)
- [x] Break `layout()` into named helpers (computeBaselineShift, findChunkRanges, buildByteIndexMappings, applyTextLength, applyTextAnchor, computeSpanBaselineShiftPx)
- [x] Add MockTextBackend unit tests for each helper
- [x] Delete `TextLayout.h/cc`

### Phase 3: TextBackendFull

- [x] Implement `TextBackendFull` wrapping HarfBuzz/FreeType calls
- [x] `TextEngine::layout()` uses capability queries (`isCursive()`, `hasSmallCapsFeature()`)
- [x] Remove `#ifdef DONNER_TEXT_FULL` code duplication from `RendererTinySkia::drawText()`
- [x] All base-config tests pass
- [x] Close text-full parity gaps
- [x] Delete `TextShaper.h/cc`
- [x] Add unit tests for `TextBackendFull`

### Phase 4: ECS caching + public API

- [x] Add `ComputedTextGeometryComponent` to the ECS registry
- [x] Add `SVGTextElement::convertToPath()`, `inkBoundingBox()`, `objectBoundingBox()`
- [x] Implement `SVGTextContentElement` public APIs on top of cached geometry
- [x] Keep `SVGTextContentElement` thin and delegate geometry/public API work to `TextEngine`
- [x] Make text public APIs prepare only the specific text root they query, not the full render tree
- [x] Add focused tests for text geometry/public API behavior
- [x] Wire invalidation to text/font/positioning property changes for `ComputedTextGeometryComponent`
- [x] Reuse `ComputedTextGeometryComponent` directly during renderer text drawing
- [x] Add broader tests for cache invalidation semantics

### Phase 5: Cleanup

- [x] Remove `fontInfo()` from `FontManager`'s public API
- [x] Replace `ImageComparisonTestFixture` table-reading helpers with a shared font metadata utility
- [x] Switch from reimplemented UTF-8 decoding to `donner/base/Utf8.h`
- [x] Move the text stack into `donner/svg/text/`
- [x] Register `TextEngine` in `registry.ctx()` for shared text measurement/layout access
- [x] Store font entities and backend caches in the same document `Registry`
- [x] Keep `TextParams` renderer-specific and use `TextLayoutParams` in the text layer
- [x] Update design docs

## Validation Status

Verification checklist at the end of this refactor:

- [x] `bazel test //...` — 50 pass, 0 fail (2026-04-04)
- [x] `bazel test //... --config=text-full` — 50 pass, 0 fail (2026-04-04)
- [ ] Historical note: broader full-Skia renderer failures existed outside the text stack
  outside the text stack (compositing, renderer image tests, resvg suite shards).

The refactored `TextEngine` + `TextBackendFull` path now matches the previous text-full
behavior for the resvg suite while removing the duplicated layout logic from `TextShaper`.
The former full-Skia renderer and `RendererTinySkia` now use `TextEngine`, and the legacy `TextShaper` source,
target, and dedicated test target have been removed. `TextLayout` and its dedicated test target
have also been removed because the production renderer no longer uses the legacy simple-layout
implementation. `FontManager` is now a font registry only and no longer exposes raw
`stbtt_fontinfo*` or backend-facing font semantics as public API. The image-comparison fixture now
uses a shared font metadata parser instead of maintaining its own inline OpenType table readers.

## File Structure (After)

```
donner/svg/text/
  TextBackend.h            # TextBackend interface, FontVMetrics, UnderlineMetrics, etc.
  TextBackendSimple.h/cc   # stb_truetype implementation of TextBackend
  TextBackendFull.h/cc     # HarfBuzz+FreeType implementation (text_full only)
  TextEngine.h/cc          # Shared SVG text layout, geometry cache, and public API support
  TextEngineHelpers.h      # Extracted layout helpers (baseline shift, chunking, textLength, text-anchor)
  TextTypes.h              # TextGlyph, TextRun (unified output types)

donner/svg/resources/
  FontManager.h/cc         # ECS-backed font service for declarations, bytes, and backend caches

donner/svg/components/
  text/ComputedTextComponent.h      # Flattened text/tree/positioning ECS state
  text/ComputedTextGeometryComponent.h  # Cached glyph geometry, character metrics, bounds
```

## Capability Model

The `TextBackend` API makes backend capabilities explicit rather than hiding them behind `#ifdef`:

| Capability | TextBackendSimple | TextBackendFull |
|---|---|---|
| `fontVMetrics()` | stbtt_GetFontVMetrics | hb_font_get_h_extents |
| `scaleForPixelHeight()` | stbtt_ScaleForPixelHeight | FT_Set_Char_Size derived |
| `underlineMetrics()` | Manual 'post' table parse | FT_Get_Sfnt_Table(post) |
| `subSuperMetrics()` | Manual OS/2 table parse | FT_Get_Sfnt_Table(OS2) |
| `glyphOutline()` | stbtt_GetGlyphShape | hb_font_draw_glyph / FT_Outline_Decompose |
| `shapeRun()` | FindGlyphIndex + HMetrics + KernAdvance | hb_shape (GSUB+GPOS) |
| `crossSpanKern()` | stbtt_GetGlyphKernAdvance | hb_shape pair technique |
| `isCursive()` | Always false | Unicode range check |
| `hasSmallCapsFeature()` | Always false | hb_ot_layout feature query |
| `bitmapGlyph()` | Always nullopt | FT_Load_Glyph + FT_LOAD_COLOR |
| `isBitmapOnly()` | FontManager::isBitmapOnly | FT_IS_SCALABLE check |

The `TextEngine` calls `isCursive()` to decide whether to suppress letter-spacing, calls
`hasSmallCapsFeature()` to decide native vs synthesized small-caps, and calls `bitmapGlyph()` with
a fallback to `glyphOutline()` — all without compile-time branching.

## ECS Caching: ComputedTextGeometryComponent

Text layout and outline extraction are expensive. Following the existing ECS "computed" pattern
(`ComputedStyleComponent`, `ComputedPathComponent`, etc.), `ComputedTextGeometryComponent` caches the
laid-out glyph geometry and per-character metrics for one text root:

```cpp
namespace donner::svg::components {

/// Cached text layout results. Attached to the text root entity.
struct ComputedTextGeometryComponent {
  struct GlyphGeometry {
    entt::entity sourceEntity = entt::null;
    Path path;
    Box2d extent;
  };

  struct CharacterGeometry {
    entt::entity sourceEntity = entt::null;
    Vector2d startPosition = Vector2d::Zero();
    Vector2d endPosition = Vector2d::Zero();
    Box2d extent;
    double rotation = 0.0;
    double advance = 0.0;
    bool rendered = false;
    bool hasExtent = false;
  };

  std::vector<GlyphGeometry> glyphs;
  std::vector<CharacterGeometry> characters;
  Box2d inkBounds;
  Box2d emBoxBounds;
};

}  // namespace donner::svg::components
```

### Invalidation

The component is invalidated (removed) via `SVGTextContentElement::invalidateTextGeometry()`, which
walks up to the text root entity and removes `ComputedTextGeometryComponent`. This is called from:

- `SVGTextContentElement`: `setTextLength`, `setLengthAdjust`, `appendText`, `advanceTextChunk`
- `SVGTextPositioningElement`: `setX/Y/Dx/Dy/Rotate` and their list variants (10 setters)
- `SVGTextPathElement`: `setHref`, `setStartOffset`

A `TextGeometry` dirty flag (`DirtyFlagsComponent::TextGeometry`) is marked alongside `RenderInstance`
on the text root entity. Font property changes (family, size, weight, style, stretch, variant)
cascade through the style system and are handled by the full render tree rebuild path.

This integrates with the existing incremental invalidation system (see
[0005-incremental_invalidation.md](../0005-incremental_invalidation.md)).

### Renderer integration

`SVGTextContentElement` and `SVGTextElement` use `ComputedTextGeometryComponent` through
`TextEngine` for public text geometry APIs. Both renderers (`RendererTinySkia`, the former full-Skia renderer)
check for cached `TextRun` data in `ComputedTextGeometryComponent` before calling `layout()`,
avoiding redundant text shaping when the cache is populated.

## Public API: Text-to-Path and Bounds

Public API consumers (editors, accessibility tools, hit-testing) need access to text geometry
without going through the renderer. New methods on `SVGTextElement`:

```cpp
class SVGTextElement : public SVGGraphicsElement {
public:
  /// Convert all text content to path outlines, suitable for export or hit-testing.
  /// Returns one Path per glyph run, with glyph positions baked in.
  std::vector<Path> convertToPath() const;

  /// Get the ink bounding box of the rendered text (actual glyph outlines).
  Box2d inkBoundingBox() const;

  /// Get the em-box bounding box (SVG objectBoundingBox for gradient mapping).
  Box2d objectBoundingBox() const;

  /// Get the number of addressable characters (for selection, caret positioning).
  int getNumberOfChars() const;

  /// Get the position of the start of a character (for caret placement).
  /// @param charIndex Zero-based index into the element's character data.
  Vector2d getStartPositionOfChar(int charIndex) const;

  /// Get the advance width/height of a character.
  /// @param charIndex Zero-based index into the element's character data.
  Vector2d getExtentOfChar(int charIndex) const;
};
```

These methods prepare only the queried text root, populate `ComputedTextGeometryComponent` if needed,
then read from the cached data. This keeps the DOM wrapper thin and avoids requiring full render
tree instantiation for text geometry queries.

### Editor use cases

| Use case | API method |
|---|---|
| Export text as paths (PDF, laser cutter) | `convertToPath()` |
| Bounding box for selection rectangle | `inkBoundingBox()` |
| Gradient mapping | `objectBoundingBox()` |
| Caret positioning | `getStartPositionOfChar(n)` |
| Character-level hit testing | `getExtentOfChar(n)` + position |
| Accessibility: character count | `getNumberOfChars()` |

## Testing Strategy

### Unit tests with MockTextBackend

The `TextBackend` interface enables testing `TextEngine` layout logic in isolation. A
`MockTextBackend` (GMock) can return controlled glyph advances, metrics, and capability flags:

```cpp
class MockTextBackend : public TextBackend {
public:
  MOCK_METHOD(FontVMetrics, fontVMetrics, (FontHandle), (const, override));
  MOCK_METHOD(float, scaleForPixelHeight, (FontHandle, float), (const, override));
  MOCK_METHOD(ShapedRun, shapeRun, (FontHandle, float, std::string_view, size_t, size_t, bool),
              (const, override));
  MOCK_METHOD(bool, isCursive, (uint32_t), (const, override));
  // ... etc.
};
```

This allows focused tests for each extracted helper:

| Test target | What it verifies |
|---|---|
| `TextEngine::positionGlyphs` | Per-character x/y/dx/dy, baseline-shift |
| `TextEngine::applyTextAnchor` | Chunk boundaries, start/middle/end shifts |
| `TextEngine::applyTextLength` | Spacing vs scaling modes |
| `TextEngine::applyTextOnPath` | Path sampling, tangent rotation |
| `TextEngine::layout` (integration) | End-to-end with mock backend |
| Cursive letter-spacing suppression | `isCursive()` → no spacing |
| Small-caps synthesis vs native | `hasSmallCapsFeature()` branches |

### Backend unit tests

Each backend gets tests against real fonts to validate its `TextBackend` implementation:

- **`TextBackendSimple` tests**: Font metrics match stb_truetype, outline paths are valid,
  kerning values are correct. Migrated from existing `FontManager_tests.cc`.
- **`TextBackendFull` tests**: Same contract tests plus HarfBuzz-specific: GSUB ligatures
  produce expected glyph IDs, cursive detection matches expected scripts, bitmap extraction
  returns valid RGBA data. Migrated from existing `TextShaper_tests.cc`.

### ComputedTextGeometryComponent tests

- Cache populated on first render, subsequent renders skip layout
- Invalidation: modifying text content / font properties / positioning clears the cache
- `inkBounds` and `emBoxBounds` are correct for test fonts
- Public API methods (`convertToPath`, `inkBoundingBox`, etc.) return consistent results with
  rendered output

### Integration / golden image tests

The existing `renderer_tests` and `resvg_test_suite` serve as end-to-end integration tests. These
run with both `--config=text-full` and the default (simple) configuration. No regressions in
pixel output should occur at any phase of the migration.

## Risks

- **Performance.** Virtual dispatch on `TextBackend` adds one indirection per glyph operation.
  This is negligible compared to the cost of shaping and path rendering. If profiling shows
  otherwise, the interface can be devirtualized via CRTP or link-time selection.
- **Subtle behavioral differences.** The two backends compute slightly different advances and
  kerning values. Unifying the layout code must not paper over these differences — the
  `shapeRun()` / `crossSpanKern()` boundary is where backend-specific behavior lives.
- **Phase ordering.** Phase 2 (shared layout extraction) is the hardest step because the two
  `layout()` methods have drifted apart. A careful diff-based approach is needed to identify
  which divergences are intentional (capability differences) vs accidental (code drift).

## TextBackendFull Parity Gaps (Resolved)

Phase 3 introduced `TextBackendFull` as a port of `TextShaper`'s HarfBuzz+FreeType code. The
following 9 resvg tests initially regressed. **All have been resolved** — the full resvg suite
passes on both `--config=text-full` and the default config as of 2026-04-04.

### Vertical CJK layout (`a-writing-mode-012`)

Japanese text (`日本`) with `writing-mode: tb` using Mplus 1p font. Diff: 11928px (threshold 600).

**Root cause:** The engine always shapes in LTR mode and uses `spanFontSizePx` as fallback
vertical advance for CJK glyphs. The old `TextShaper` shaped with `HB_DIRECTION_TTB` and used
HarfBuzz's vertical metrics directly. For CJK fonts with proper vmtx/vhea tables, the advance
from vertical shaping may differ from the em-height fallback.

**Fix plan:** For upright CJK glyphs in vertical mode, query the font's vertical advance from
HarfBuzz using `hb_font_get_glyph_v_advance()` instead of falling back to em height. This can
be done either in `TextBackendFull::shapeRun()` (shape each CJK segment in TTB) or by adding a
`glyphVAdvance()` method to `TextBackend`.

### Emoji bitmap glyphs (`e-text-027`, `e-text-028`, `e-text-029`)

Color emoji rendering using Noto Color Emoji with CBDT/CBLC bitmaps. Diffs: 17080, 4075, 11942.

**Root cause:** The `TextBackendFull::bitmapGlyph()` implementation was ported from `TextShaper`
but the rendering pipeline changed: the old `TextShaper` rendered bitmaps through its own path
(`shaper.bitmapGlyph()`), now `TextBackendFull::bitmapGlyph()` is called from the renderer. The
bitmap strike selection, scale computation, or bearing offsets may differ.

**Fix plan:** Debug by comparing the `BitmapGlyph` struct output from the old `TextShaper` vs
the new `TextBackendFull` for the same glyph. Check strike ppem, scale factor, and
bearing X/Y values. The renderer's bitmap drawing code is unchanged so the issue is in the
backend's extraction.

### Per-character rotation with patterns (`e-text-033`, `e-text-034`, `e-text-036`)

Tests with `rotate` attribute and complex text (diacritics, Arabic). Diffs: 775, 1914, 4872.

**Root cause:** The old `TextShaper` had its own rotation/combining-mark logic interleaved with
HarfBuzz cluster processing. The new `TextEngine` decodes codepoints from cluster byte offsets
and applies rotation. For multi-glyph clusters (decomposed characters, Arabic joining), the
cluster→charIdx mapping may differ from the old TextShaper's byte-level `isSmallCap` tracking.

**Fix plan:** Compare per-glyph `rotateDegrees` between old and new paths for these test SVGs.
The likely fix is in the `charIdx` advancement logic when HarfBuzz produces multiple glyphs for
a single cluster (mark attachment) or fewer glyphs than codepoints (ligatures).

### Underline with text-rendering (`a-text-rendering-005`)

Underline rendering. Diff: 1137 (threshold 100).

**Root cause:** The `TextBackendFull::underlineMetrics()` reads `ftFace->underline_position` and
`ftFace->underline_thickness`, which are in font design units (same convention as the simple
backend's post table parsing). However, the values may differ from what the old TextShaper
computed (which read the post table directly via raw FreeType table access). The FreeType fields
may apply additional transformations.

**Fix plan:** Compare the underline position/thickness values between old and new paths. If the
FreeType accessors differ from raw post table values, switch to `FT_Get_Sfnt_Table(FT_SFNT_POST)`
for direct access, matching the simple backend's approach.

### tspan bbox (`e-tspan-030`)

tspan bounding box test. Diff: 933 (threshold 400).

**Root cause:** The text bounding box (em-box bounds) computation in the renderer uses
`backend.fontVMetrics()` and `backend.scaleForPixelHeight()`. For the full backend, `fontVMetrics`
reads `FT_Face->ascender/descender` while the simple backend reads stb_truetype's metrics. These
may differ for the same font if FreeType and stb_truetype interpret the font tables differently
(e.g., hhea vs OS/2 metrics).

**Fix plan:** Compare `fontVMetrics()` output between backends for the test font. Ensure both
return the same ascent/descent values. If they differ, determine which is correct per the
OpenType spec and align both backends.
