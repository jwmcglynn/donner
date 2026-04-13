# Text Rendering: RTL Text and Complex Scripts

[Back to hub](../0010-text_rendering.md)

## RTL Text and Complex Scripts (Phase 7) {#rtl}

**Status:** Partially implemented
**Prerequisite:** text-full (HarfBuzz + FreeType)

### Current State

- HarfBuzz auto-detects script and direction from text content (Arabic → RTL, Latin → LTR).
- GSUB joining forms work correctly — Arabic characters connect properly in the top line of e-text-030 (no per-character coordinates).
- Per-character coordinate lists are handled for the covered text-full RTL cases.
- Remaining major gap: full Unicode BiDi reordering for mixed-direction paragraphs and spans.

### Problem: RTL Pen Direction

For LTR text, the pen starts at `x` and moves RIGHT via positive `x_advance`:
```
penX = x
for each glyph:
    draw at penX
    penX += x_advance  // positive, moves right
```

For RTL text with HarfBuzz, glyphs are output in VISUAL order (left to right for rendering), but the pen should still logically move from the START position rightward while the glyphs are placed right-to-left. HarfBuzz handles this by:
1. Outputting glyphs in visual order
2. Giving positive `x_advance` values (width of each glyph)
3. The caller is responsible for RTL layout

The SVG spec says `x` specifies the position of the first character's start edge. For RTL, the first character's start edge is on the RIGHT. So `x=50` means the rightmost character starts at x=50.

### What's Done

- **Script auto-detection**: Removed hardcoded `HB_DIRECTION_LTR` / `HB_SCRIPT_LATIN`. Now `hb_buffer_guess_segment_properties()` auto-detects script and direction. Arabic GSUB joining forms (connected cursive) now work correctly.
- **Basic RTL layout**: HarfBuzz outputs glyphs in visual order (left-to-right) with positive `x_advance`. The existing pen-based loop works without modification — `penX` starts at `x` (which SVG defines as the start of text, NOT the right edge for RTL) and advances right through visual glyphs. Tested: top line of e-text-030 renders correctly.

### Per-character coordinates with RTL (Done)

**Problem**: When `y="140 150 160 170"` is applied to RTL text "الويب", the glyph loop processes
glyphs in visual (LTR) order, but the y values should be applied in DOM order per Chrome/resvg.

**Solution implemented**: When RTL text has per-character x/y coordinates, `TextEngine` with
`TextBackendFull` switches to logical-order chunk handling for coordinate application:
- Chunks are built in logical (DOM) order instead of visual order
- Single-character chunks shape with `HB_DIRECTION_LTR`; multi-character chunks keep RTL for correct Arabic joining
- `posIdx` uses `charIdx` (logical) instead of `gi` (visual) for coordinate lookups
- Multi-character RTL chunks share the chunk's explicit Y via `chunkYOverrides` map

#### Additional fixes in this area

- **Chunk splitting uses UTF-16 indices** (not raw codepoint indices) for `xList`/`yList` lookups,
  preventing ZWJ emoji sequences from being split.
- **First logical character** recognized via `span.startsNewChunk` or `xList[0]`/`yList[0]` having
  values, ensuring correct chunk boundaries.
- **Rotate uses raw codepoint indices** — combining marks consume rotate values (matching Chrome/resvg),
  unlike x/y which use addressable character indices. Separate `byteToRawCpIdx` map.
- **Combining mark rotation** — marks' positions are pre-rotated around the base character's origin
  so grapheme clusters rotate as a unit.
- **FT_LOAD_NO_HINTING** for glyph outline extraction, matching HarfBuzz font configuration.
- **Cross-family font fallback** — when the primary font lacks glyphs for a script, tries all
  registered fonts to find one with coverage.
- **Nested `<text>` rejection** — `TextSystem` skips nested `<text>` elements (invalid per SVG spec).
- **Cross-span kerning suppression** — `TextLayout` resets `prevGlyph` when a span starts a new
  text chunk, preventing stale kerning from affecting explicit positions.

#### Test Coverage

- `e-text-029`: Compound emoji with per-character coordinates (ZWJ sequences)
- `e-text-030`: Arabic text with per-character y coordinates (custom golden)
- `e-text-034`: Rotate with complex grapheme clusters (combining marks)
- `e-text-036`: Arabic text with rotate and font fallback (custom golden, Amiri)
- `e-text-037`: Nested `<text>` elements (should render nothing)
- `e-text-040`: Arabic text with fill-rule=evenodd

## Color Emoji (Phase 8) {#color-emoji}

**Status:** Implemented (text-full only)
**See also:** [0006-color_emoji.md](../0006-color_emoji.md)

CBDT/CBLC color bitmap emoji support via FreeType. Enabled by adding libpng to the FreeType build
via `single_version_override` patches. Key components:
- FontManager accepts bitmap-only fonts (no glyf table)
- TextShaper extracts BGRA bitmaps via `FT_Load_Glyph(FT_LOAD_COLOR)`
- RendererTinySkia renders bitmaps as scaled images via `drawPixmap`
- UTF-16 code unit indexing for per-character coordinate lists (matching SVG DOM)
- Low surrogate coordinate consumption for supplementary characters
