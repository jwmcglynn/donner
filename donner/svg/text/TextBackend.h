#pragma once
/// @file

#include <optional>

#include "donner/svg/core/FontVariant.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::svg {

/// Font vertical metrics in font design units (unscaled).
struct FontVMetrics {
  int ascent = 0;   ///< Positive, above baseline.
  int descent = 0;  ///< Negative, below baseline.
  int lineGap = 0;
};

/// Line decoration positioning metrics in font design units.
struct UnderlineMetrics {
  double position = 0;   ///< Signed offset from baseline in font design units (negative = below).
  double thickness = 0;  ///< Stroke thickness in font design units.
};

/// Sub/superscript Y offsets from the OS/2 table, in font design units.
struct SubSuperMetrics {
  int subscriptYOffset = 0;
  int superscriptYOffset = 0;
};

/**
 * Abstract font backend for text rendering operations.
 *
 * Abstracts all font-engine-specific calls (stb_truetype, HarfBuzz+FreeType) behind a
 * uniform interface. Used by TextEngine for layout and by renderers for glyph outlines
 * and font metrics.
 *
 * Implementations:
 * - TextBackendSimple: stb_truetype (no GSUB/GPOS, no bitmap glyphs)
 * - TextBackendFull: HarfBuzz + FreeType (full OpenType shaping)
 */
class TextBackend {
public:
  virtual ~TextBackend() = default;

  // ── Font metrics ──────────────────────────────────────────────

  /// Vertical metrics (ascent, descent, lineGap) in font design units.
  virtual FontVMetrics fontVMetrics(FontHandle font) const = 0;

  /// Scale factor: font design units → pixels, normalized to ascent−descent height.
  /// This is `stbtt_ScaleForPixelHeight` semantics.
  virtual float scaleForPixelHeight(FontHandle font, float pixelHeight) const = 0;

  /// Scale factor: em units → pixels. Differs from scaleForPixelHeight when
  /// ascent−descent != unitsPerEm. This is the correct scale for CSS font-size.
  virtual float scaleForEmToPixels(FontHandle font, float pixelHeight) const = 0;

  // ── Table-derived metrics ─────────────────────────────────────

  /// Underline position/thickness from the 'post' table, in font design units.
  /// Returns std::nullopt if the table is missing.
  virtual std::optional<UnderlineMetrics> underlineMetrics(FontHandle font) const = 0;

  /// Strikeout position/thickness from the OS/2 table, in font design units.
  /// Returns std::nullopt if the table is missing.
  virtual std::optional<UnderlineMetrics> strikeoutMetrics(FontHandle font) const = 0;

  /// Sub/superscript Y offsets from the OS/2 table, in font design units.
  /// Returns std::nullopt if the table is missing.
  virtual std::optional<SubSuperMetrics> subSuperMetrics(FontHandle font) const = 0;

  // ── Glyph operations ─────────────────────────────────────────

  /// Extract a glyph outline as a PathSpline. Coordinates are in font units
  /// scaled by \p scale, with Y flipped for SVG's y-down convention.
  virtual PathSpline glyphOutline(FontHandle font, int glyphIndex, float scale) const = 0;

  /// Returns true if the font is bitmap-only (no vector outlines).
  virtual bool isBitmapOnly(FontHandle font) const = 0;

  // ── Capability queries ────────────────────────────────────────

  /// Returns true if letter-spacing should be suppressed for this codepoint
  /// (cursive/connected scripts like Arabic). Simple backends return false.
  virtual bool isCursive(uint32_t codepoint) const = 0;

  /// Returns true if the font has a native OpenType small-caps feature (smcp).
  /// If false, the engine synthesizes small-caps via uppercase + reduced size.
  virtual bool hasSmallCapsFeature(FontHandle font) const = 0;

  // ── Bitmap glyphs (optional) ──────────────────────────────────

  /// Bitmap glyph data from color fonts (CBDT/CBLC).
  struct BitmapGlyph {
    std::vector<uint8_t> rgbaPixels;  ///< RGBA pixel data (premultiplied alpha).
    int width = 0;                    ///< Bitmap width in pixels.
    int height = 0;                   ///< Bitmap height in pixels.
    double bearingX = 0;              ///< Horizontal offset from glyph origin.
    double bearingY = 0;              ///< Vertical offset from baseline (positive = up).
    double scale = 1.0;               ///< Scale factor to apply.
  };

  /// Extract a bitmap glyph (CBDT/CBLC color emoji). Returns std::nullopt if
  /// the glyph is not a bitmap. Simple backends always return std::nullopt.
  virtual std::optional<BitmapGlyph> bitmapGlyph(FontHandle font, int glyphIndex,
                                                 float scale) const = 0;

  // ── Shaping ───────────────────────────────────────────────────

  /// A single shaped glyph with advance and cluster info.
  struct ShapedGlyph {
    int glyphIndex = 0;          ///< Backend-specific glyph ID.
    double xAdvance = 0;         ///< Nominal horizontal advance (without kerning).
    double yAdvance = 0;         ///< Nominal vertical advance (without kerning).
    double xKern = 0;            ///< Kerning adjustment to apply before this glyph.
    double yKern = 0;            ///< Kerning adjustment (vertical mode).
    double xOffset = 0;          ///< Horizontal offset (GPOS mark positioning; 0 for simple).
    double yOffset = 0;          ///< Vertical offset (GPOS mark positioning; 0 for simple).
    uint32_t cluster = 0;        ///< Byte offset into the shaped text range.
    float fontSizeScale = 1.0f;  ///< < 1.0 for synthesized small-caps glyphs.
  };

  /// Result of shaping a text range.
  struct ShapedRun {
    std::vector<ShapedGlyph> glyphs;
  };

  /// Shape a range of text, producing glyph IDs with advances and kerning.
  ///
  /// The full span text and a byte range within it are provided so context-aware
  /// backends (HarfBuzz) can use surrounding context for GSUB/GPOS.
  ///
  /// @param font       Font to shape with.
  /// @param fontSizePx Font size in pixels.
  /// @param spanText   Full span text (UTF-8).
  /// @param byteOffset Start of the range to shape within \p spanText.
  /// @param byteLength Length of the range to shape.
  /// @param isVertical True for vertical writing mode.
  /// @param fontVariant Font variant (Normal or SmallCaps).
  /// @param forceLogicalOrder If true, return glyphs sorted by cluster (DOM order)
  ///        rather than visual order. Used for per-character positioned RTL text.
  /// @return Shaped glyphs with advances, kerning, and cluster mapping.
  virtual ShapedRun shapeRun(FontHandle font, float fontSizePx, std::string_view spanText,
                             size_t byteOffset, size_t byteLength, bool isVertical,
                             FontVariant fontVariant, bool forceLogicalOrder) const = 0;

  /// Compute cross-span kerning between the last codepoint of the previous span
  /// and the first codepoint of the current span.
  /// @return Kern adjustment in pixels (added to pen position).
  virtual double crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle curFont,
                               float curSizePx, uint32_t prevCodepoint, uint32_t curCodepoint,
                               bool isVertical) const = 0;
};

}  // namespace donner::svg
