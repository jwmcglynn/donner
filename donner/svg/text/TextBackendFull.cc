#include "donner/svg/text/TextBackendFull.h"

#include <ft2build.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>

#include "donner/base/Utf8.h"
#include "donner/svg/text/BitmapGlyphUtils.h"

namespace donner::svg {

namespace {

int16_t ReadInt16Be(std::span<const uint8_t> data, size_t offset) {
  return static_cast<int16_t>(
      static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]));
}

/// Read the exact design-unit denominator without a pixel-scale round trip.
int ReadUnitsPerEm(const FontManager& manager, FontHandle font) {
  const auto head = manager.sfntTable(font, "head");
  return head && head->size() >= 20 ? static_cast<uint16_t>(ReadInt16Be(*head, 18)) : 0;
}

/// Lazily-initialized FreeType library (process-global, never destroyed).
FT_Library getFtLibrary() {
  static FT_Library lib = [] {
    FT_Library l = nullptr;
    FT_Init_FreeType(&l);
    return l;
  }();
  return lib;
}

/// Returns true if the codepoint belongs to a cursive/joining script where letter-spacing
/// should be suppressed (CSS Text §8.1: "cursive scripts ... must not use letter-spacing").
/// This covers Arabic, Syriac, N'Ko, Mandaic, and similar connecting scripts.
bool isCursiveScript(uint32_t cp) {
  // Arabic (0600-06FF), Arabic Supplement (0750-077F), Arabic Extended-A (08A0-08FF),
  // Arabic Presentation Forms-A (FB50-FDFF), Arabic Presentation Forms-B (FE70-FEFF).
  if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
      (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
      (cp >= 0xFE70 && cp <= 0xFEFF)) {
    return true;
  }
  // Syriac (0700-074F), Syriac Supplement (0860-086F).
  if ((cp >= 0x0700 && cp <= 0x074F) || (cp >= 0x0860 && cp <= 0x086F)) {
    return true;
  }
  // N'Ko (07C0-07FF).
  if (cp >= 0x07C0 && cp <= 0x07FF) {
    return true;
  }
  // Mandaic (0840-085F).
  if (cp >= 0x0840 && cp <= 0x085F) {
    return true;
  }
  return false;
}

bool HasCachedOutlineTables(const FontManager& fontManager, FontHandle font) {
  return fontManager.sfntTable(font, "glyf").has_value() ||
         fontManager.sfntTable(font, "CFF ").has_value() ||
         fontManager.sfntTable(font, "CFF2").has_value();
}

bool CanDecodeGlyphOutline(const FontManager& fontManager, FontHandle font, int glyphIndex,
                           float scale) {
  return glyphIndex >= 0 && std::isfinite(scale) && scale >= 0.0f &&
         (fontManager.isTrustedFont(font) ||
          fontManager.glyphOutlineComplexity(font, glyphIndex).has_value());
}

/// Measure the lowercase x top in unscaled design units when the font omits sxHeight.
int MeasureXHeight(const FontManager& manager, FontHandle handle, hb_font_t* font) {
  if (!font) {
    return 0;
  }
  FT_Face face = hb_ft_font_get_ft_face(font);
  if (!face) {
    return 0;
  }
  const FT_UInt glyph = FT_Get_Char_Index(face, 'x');
  if (glyph == 0 || !CanDecodeGlyphOutline(manager, handle, static_cast<int>(glyph), 1.0f)) {
    return 0;
  }
  if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
    return 0;
  }
  const FT_Pos top = face->glyph->metrics.horiBearingY;
  return top > 0 && top <= std::numeric_limits<int>::max() ? static_cast<int>(top) : 0;
}

/// Checks the exclusive fixed-point bound before converting to FreeType's signed integer type.
std::optional<FT_F26Dot6> CheckedCharacterSize(float sizePx) {
  const double fixedSize = static_cast<double>(sizePx) * 64.0;
  // The integer maximum can round upward to an out-of-range power of two in floating point.
  const double exclusiveLimit = std::ldexp(1.0, std::numeric_limits<FT_F26Dot6>::digits);
  if (!std::isfinite(fixedSize) || sizePx <= 0.0f || fixedSize >= exclusiveLimit) {
    return std::nullopt;
  }
  return static_cast<FT_F26Dot6>(fixedSize);
}

/// Sets a scalable face's checked size and refreshes HarfBuzz only after FreeType accepts it.
bool SetScalableFaceSize(hb_font_t* font, FT_Face face, float sizePx) {
  const auto characterSize = CheckedCharacterSize(sizePx);
  if (!face || !characterSize) {
    return false;
  }
  if (FT_IS_SCALABLE(face)) {
    if (FT_Set_Char_Size(face, 0, *characterSize, 72, 72) != 0) {
      return false;
    }
    hb_ft_font_changed(font);
  }
  return true;
}

/// Prepares either a scalable size or the existing bitmap strike for a shaping operation.
bool PrepareShapingSize(hb_font_t* font, FT_Face face, float sizePx) {
  if (!SetScalableFaceSize(font, face, sizePx)) {
    return false;
  }
  if (!FT_IS_SCALABLE(face)) {
    if (face->num_fixed_sizes <= 0 || FT_Select_Size(face, 0) != 0) {
      return false;
    }
    hb_ft_font_changed(font);
  }
  return true;
}

std::optional<float> CheckedFontSizePx(float scale, unsigned int unitsPerEm) {
  const double fontSizePx = static_cast<double>(scale) * unitsPerEm;
  if (!std::isfinite(fontSizePx) || fontSizePx <= 0.0 ||
      fontSizePx > static_cast<double>(std::numeric_limits<float>::max())) {
    return std::nullopt;
  }
  const float result = static_cast<float>(fontSizePx);
  return CheckedCharacterSize(result) ? std::optional<float>(result) : std::nullopt;
}

/// Small-caps synthesis scale factor.
constexpr float kSmallCapScale = 0.8f;

/// Decode a single UTF-8 codepoint from a byte offset in a string.
uint32_t decodeCodepointAt(std::string_view str, size_t byteOffset) {
  if (byteOffset >= str.size()) {
    return 0;
  }

  const auto [cp, /*length*/ _] = Utf8::NextCodepoint(str.substr(byteOffset));
  return static_cast<uint32_t>(cp);
}

double pixelScaleForPpem(FT_Face ftFace, float fontSizePx, bool useXAxis) {
  if (!ftFace || !ftFace->size) {
    return 1.0 / 64.0;
  }

  const unsigned int ppem = useXAxis ? ftFace->size->metrics.x_ppem : ftFace->size->metrics.y_ppem;
  if (ppem == 0) {
    return 1.0 / 64.0;
  }

  return static_cast<double>(fontSizePx) / (static_cast<double>(ppem) * 64.0);
}

}  // namespace

// ---------------------------------------------------------------------------
// HbFontEntry
// ---------------------------------------------------------------------------

/// Internal storage for a HarfBuzz font object backed by FreeType.
/// The hb_font_t is created via hb_ft_font_create_referenced(), which holds a reference to the
/// FT_Face. We also hold our own reference (from FT_New_Memory_Face). Destruction order: destroy
/// hb_font first (decrefs FT_Face from 2->1), then FT_Done_Face (decrefs 1->0, frees).
struct TextBackendFull::HbFontEntry {
  HbFontEntry() = default;
  HbFontEntry(const HbFontEntry&) = delete;
  HbFontEntry& operator=(const HbFontEntry&) = delete;

  HbFontEntry(HbFontEntry&& other) noexcept
      : font(other.font),
        ftFace(other.ftFace),
        syntheticBold(other.syntheticBold),
        syntheticOblique(other.syntheticOblique) {
    other.font = nullptr;
    other.ftFace = nullptr;
  }

  HbFontEntry& operator=(HbFontEntry&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    if (font) {
      hb_font_destroy(font);
    }
    if (ftFace) {
      FT_Done_Face(ftFace);
    }

    font = other.font;
    ftFace = other.ftFace;
    syntheticBold = other.syntheticBold;
    syntheticOblique = other.syntheticOblique;
    other.font = nullptr;
    other.ftFace = nullptr;
    return *this;
  }

  hb_font_t* font = nullptr;
  FT_Face ftFace = nullptr;
  bool syntheticBold = false;
  bool syntheticOblique = false;

  ~HbFontEntry() {
    if (font) {
      hb_font_destroy(font);
    }
    if (ftFace) {
      FT_Done_Face(ftFace);
    }
  }
};

namespace {

bool CanCreateHbFont(const FontManager& fontManager, FontHandle handle) {
  return handle && fontManager.isValidatedFont(handle) &&
         (fontManager.isTrustedFont(handle) || HasCachedOutlineTables(fontManager, handle));
}

void SetInitialFaceSize(FT_Face face) {
  if (face->num_fixed_sizes > 0 && !FT_IS_SCALABLE(face)) {
    FT_Select_Size(face, 0);
  } else {
    FT_Set_Char_Size(face, 0, 16 * 64, 72, 72);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TextBackendFull::TextBackendFull(FontManager& fontManager, Registry& registry)
    : fontManager_(fontManager), registry_(registry) {}

TextBackendFull::~TextBackendFull() = default;

bool TextBackendFull::embeddedBitmapLoadingDisabledForTesting(FontHandle font) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  return hbFont && (hb_ft_font_get_load_flags(hbFont) & FT_LOAD_NO_BITMAP) != 0;
}

namespace {

FT_Int32 FreeTypeLoadFlags(const FontManager& fontManager, FontHandle font) {
  return FT_LOAD_NO_HINTING |
         (fontManager.isTrustedFont(font) ? FT_Int32{0} : FT_Int32{FT_LOAD_NO_BITMAP});
}

constexpr FT_ULong Tag(char a, char b, char c, char d) {
  return (static_cast<FT_ULong>(a) << 24) | (static_cast<FT_ULong>(b) << 16) |
         (static_cast<FT_ULong>(c) << 8) | static_cast<FT_ULong>(d);
}

struct AppliedVariationAxes {
  bool weight = false;
  bool style = false;
};

AppliedVariationAxes SetRequestedVariationAxes(FT_Library library, FT_Face face,
                                               const FontFaceRequest& request) {
  if (!FT_HAS_MULTIPLE_MASTERS(face)) {
    return {};
  }
  FT_MM_Var* variation = nullptr;
  if (FT_Get_MM_Var(face, &variation) != 0 || variation == nullptr) {
    return {};
  }
  AppliedVariationAxes applied;
  constexpr FT_UInt kMaximumAxes = 32;
  if (variation->num_axis <= kMaximumAxes) {
    std::vector<FT_Fixed> coordinates;
    coordinates.reserve(variation->num_axis);
    for (FT_UInt index = 0; index < variation->num_axis; ++index) {
      const FT_Var_Axis& axis = variation->axis[index];
      FT_Fixed value = axis.def;
      if (axis.tag == Tag('w', 'g', 'h', 't')) {
        value = static_cast<FT_Fixed>(std::clamp(request.weight, 1, 1000)) << 16;
        applied.weight = true;
      } else if (axis.tag == Tag('i', 't', 'a', 'l')) {
        value = request.style == FontStyle::Normal ? 0 : (1 << 16);
        applied.style = true;
      } else if (axis.tag == Tag('s', 'l', 'n', 't') && request.style != FontStyle::Normal) {
        value = -(10 << 16);
        applied.style = true;
      }
      coordinates.push_back(std::clamp(value, axis.minimum, axis.maximum));
    }
    if (FT_Set_Var_Design_Coordinates(face, variation->num_axis, coordinates.data()) != 0) {
      applied = {};
    }
  }
  FT_Done_MM_Var(library, variation);
  return applied;
}

void ConfigureProviderFace(const FontManager& fontManager, FontHandle handle, FT_Face face,
                           bool* syntheticBold, bool* syntheticOblique) {
  const auto request = fontManager.providerFaceRequest(handle);
  if (!request.has_value()) {
    return;
  }
  const AppliedVariationAxes axes = SetRequestedVariationAxes(getFtLibrary(), face, *request);
  const bool catalog = fontManager.isImmutableCatalogFont(handle);
  const bool generic = fontManager.isGenericSansProviderFont(handle);
  const bool intrinsicBold = (face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
  const bool intrinsicItalic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
  *syntheticBold = (catalog || generic) && request->weight >= 600 && !axes.weight && !intrinsicBold;
  if (request->style != FontStyle::Normal && !axes.style && !intrinsicItalic &&
      (catalog || generic)) {
    FT_Matrix italicFromUpright{1 << 16, static_cast<FT_Fixed>(0.22 * (1 << 16)), 0, 1 << 16};
    FT_Set_Transform(face, &italicFromUpright, nullptr);
    *syntheticOblique = true;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Font cache
// ---------------------------------------------------------------------------

hb_font_t* TextBackendFull::getOrCreateHbFont(FontHandle handle) const {
  if (!handle) {
    return nullptr;
  }
  if (const auto* entry = registry_.try_get<HbFontEntry>(handle.entity())) {
    return entry->font;
  }
  if (!CanCreateHbFont(fontManager_, handle)) {
    // Bitmap-only fonts can cause FreeType to decompress embedded PNG strikes from HarfBuzz
    // metric callbacks. Reject document-provided bitmap fonts before constructing an FT_Face.
    return nullptr;
  }

  // Create a FreeType face from the raw font data, then wrap it with HarfBuzz. This exact
  // whole-font span is also the safe CFF boundary; unlike stb_truetype, FreeType does not invent a
  // larger CFF table span past the caller-owned allocation.
  const auto data = fontManager_.fontData(handle);
  if (data.empty() || data.size() > static_cast<size_t>(std::numeric_limits<FT_Long>::max())) {
    return nullptr;
  }

  auto& entry = registry_.emplace<HbFontEntry>(handle.entity());

  // FT_New_Memory_Face does NOT copy the data - it keeps a pointer. The font data is owned by
  // FontManager (via shared_ptr), so it outlives the FT_Face.
  FT_Error err = FT_New_Memory_Face(getFtLibrary(), data.data(), static_cast<FT_Long>(data.size()),
                                    0, &entry.ftFace);
  if (err != 0) {
    return nullptr;
  }
  ConfigureProviderFace(fontManager_, handle, entry.ftFace, &entry.syntheticBold,
                        &entry.syntheticOblique);

  // Set a default size (will be overridden per shaping call).
  // For bitmap-only fonts (CBDT), use FT_Select_Size to pick the first strike;
  // FT_Set_Char_Size fails for non-scalable fonts.
  SetInitialFaceSize(entry.ftFace);

  // Create HarfBuzz font backed by FreeType for GSUB/GPOS shaping.
  entry.font = hb_ft_font_create_referenced(entry.ftFace);
  if (!entry.font) {
    return nullptr;
  }

  // Disable hinting for glyph outline extraction so outlines match raw font data
  // (consistent with stb_truetype and resvg's ttf-parser). Hinting changes glyph
  // proportions, causing width/height mismatches against reference renderers.
  hb_ft_font_set_load_flags(entry.font, FreeTypeLoadFlags(fontManager_, handle));
  return entry.font;
}

// ---------------------------------------------------------------------------
// Font metrics
// ---------------------------------------------------------------------------

FontVMetrics TextBackendFull::fontVMetrics(FontHandle font) const {
  // Read from the raw font data's hhea table to match stb_truetype's behavior.
  // FreeType's FT_Face->ascender may come from OS/2 instead of hhea, causing mismatches.
  if (!fontManager_.isValidatedFont(font)) {
    return {};
  }

  const auto hhea = fontManager_.sfntTable(font, "hhea");
  if (hhea && hhea->size() >= 10) {
    FontVMetrics metrics;
    metrics.ascent = ReadInt16Be(*hhea, 4);
    metrics.descent = ReadInt16Be(*hhea, 6);
    metrics.lineGap = ReadInt16Be(*hhea, 8);
    metrics.unitsPerEm = ReadUnitsPerEm(fontManager_, font);

    // x-height from the OS/2 table (`sxHeight`, offset 86), present in version >= 2.
    const auto os2 = fontManager_.sfntTable(font, "OS/2");
    if (os2 && os2->size() >= 88) {
      const uint16_t os2Version = static_cast<uint16_t>(ReadInt16Be(*os2, 0));
      if (os2Version >= 2) {
        metrics.xHeight = ReadInt16Be(*os2, 86);
      }
    }
    if (metrics.xHeight <= 0) {
      metrics.xHeight = MeasureXHeight(fontManager_, font, getOrCreateHbFont(font));
    }
    return metrics;
  }

  // Fallback to FreeType if hhea not found.
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return {};
  }
  FT_Face ftFace = hb_ft_font_get_ft_face(hbFont);
  if (!ftFace) {
    return {};
  }
  FontVMetrics metrics;
  metrics.ascent = ftFace->ascender;
  metrics.descent = ftFace->descender;
  metrics.lineGap = ftFace->height - (ftFace->ascender - ftFace->descender);
  metrics.unitsPerEm = ftFace->units_per_EM;
  metrics.xHeight = MeasureXHeight(fontManager_, font, hbFont);
  return metrics;
}

float TextBackendFull::scaleForPixelHeight(FontHandle font, float pixelHeight) const {
  // Match FontManager::scaleForPixelHeight semantics: em-based scaling.
  // pixelHeight / units_per_em maps 1 em to the given pixel height.
  return scaleForEmToPixels(font, pixelHeight);
}

float TextBackendFull::scaleForEmToPixels(FontHandle font, float pixelHeight) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return 0.0f;
  }

  // Use HarfBuzz's upem (from the head table) instead of FreeType's units_per_EM,
  // because FreeType sets units_per_EM = 0 for non-scalable (bitmap-only) fonts like
  // CBDT color emoji. HarfBuzz reads the head table independently.
  const unsigned int upem = hb_face_get_upem(hb_font_get_face(hbFont));
  if (upem == 0) {
    return 0.0f;
  }

  return pixelHeight / static_cast<float>(upem);
}

// ---------------------------------------------------------------------------
// Table-derived metrics
// ---------------------------------------------------------------------------

std::optional<UnderlineMetrics> TextBackendFull::underlineMetrics(FontHandle font) const {
  // Read from the raw 'post' table to match TextBackendSimple's behavior exactly.
  // FreeType's FT_Face->underline_position may differ from the raw table values.
  if (!fontManager_.isValidatedFont(font)) {
    return std::nullopt;
  }

  const auto post = fontManager_.sfntTable(font, "post");
  if (!post || post->size() < 12) {
    return std::nullopt;
  }

  UnderlineMetrics metrics;
  metrics.position = static_cast<double>(ReadInt16Be(*post, 8));
  metrics.thickness = static_cast<double>(ReadInt16Be(*post, 10));
  return metrics;
}

std::optional<UnderlineMetrics> TextBackendFull::strikeoutMetrics(FontHandle font) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return std::nullopt;
  }

  FT_Face ftFace = hb_ft_font_get_ft_face(hbFont);
  if (!ftFace) {
    return std::nullopt;
  }

  auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(ftFace, FT_SFNT_OS2));
  if (!os2) {
    return std::nullopt;
  }

  UnderlineMetrics metrics;
  metrics.position = static_cast<double>(os2->yStrikeoutPosition);
  metrics.thickness = static_cast<double>(os2->yStrikeoutSize);
  return metrics;
}

std::optional<SubSuperMetrics> TextBackendFull::subSuperMetrics(FontHandle font) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return std::nullopt;
  }

  FT_Face ftFace = hb_ft_font_get_ft_face(hbFont);
  if (!ftFace) {
    return std::nullopt;
  }

  auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(ftFace, FT_SFNT_OS2));
  if (!os2) {
    return std::nullopt;
  }

  SubSuperMetrics metrics;
  metrics.subscriptYOffset = os2->ySubscriptYOffset;
  metrics.superscriptYOffset = os2->ySuperscriptYOffset;
  return metrics;
}

// ---------------------------------------------------------------------------
// Glyph outline
// ---------------------------------------------------------------------------

int TextBackendFull::outlineLoadFlags(FontHandle font, bool allowSyntheticOblique) const {
  const auto* entry = registry_.try_get<HbFontEntry>(font.entity());
  const bool suppressOblique = !allowSyntheticOblique && entry && entry->syntheticOblique;
  return FreeTypeLoadFlags(fontManager_, font) | (suppressOblique ? FT_LOAD_IGNORE_TRANSFORM : 0);
}

Path TextBackendFull::glyphOutline(FontHandle font, int glyphIndex, float scale) const {
  return glyphOutline(font, glyphIndex, scale, true);
}

Path TextBackendFull::glyphOutline(FontHandle font, int glyphIndex, float scale,
                                   bool allowSyntheticOblique) const {
  if (!CanDecodeGlyphOutline(fontManager_, font, glyphIndex, scale)) {
    return {};
  }
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return {};
  }

  // The `scale` parameter is fontSizePx / upem (same as stbtt_ScaleForMappingEmToPixels).
  // Convert back to fontSizePx for FreeType.
  const unsigned int upem = hb_face_get_upem(hb_font_get_face(hbFont));
  const std::optional<float> fontSizePx = CheckedFontSizePx(scale, upem);
  if (!fontSizePx.has_value()) {
    return {};
  }

  // Get the FreeType face and set it to the correct size.
  FT_Face ftFace = hb_ft_font_get_ft_face(hbFont);
  if (!SetScalableFaceSize(hbFont, ftFace, *fontSizePx)) {
    return {};
  }

  // Use NO_HINTING to match the HarfBuzz font configuration (set in getOrCreateHbFont).
  // Hinted outlines differ from unhinted metrics, causing shape/position mismatches
  // especially for composite glyphs (precomposed accented characters like e).
  if (FT_Load_Glyph(ftFace, static_cast<FT_UInt>(glyphIndex),
                    outlineLoadFlags(font, allowSyntheticOblique)) != 0) {
    return {};
  }

  if (ftFace->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
    return {};
  }
  if (const auto* entry = registry_.try_get<HbFontEntry>(font.entity());
      entry && entry->syntheticBold) {
    const FT_Pos strength =
        static_cast<FT_Pos>(std::clamp(*fontSizePx * 0.03f * 64.0f, 0.0f, 4096.0f));
    FT_Outline_Embolden(&ftFace->glyph->outline, strength);
  }

  // Decompose the outline into a Path via PathBuilder.
  // FreeType outline coordinates are in 26.6 fixed-point pixels at the set size.
  // Dividing by 64 gives the pixel coordinates, which match the caller's expected scale
  // because we set FreeType's size to `scale * upem`.
  PathBuilder builder;
  const double ftScaleX = pixelScaleForPpem(ftFace, *fontSizePx, true);
  const double ftScaleY = pixelScaleForPpem(ftFace, *fontSizePx, false);

  FT_Outline_Funcs funcs{};
  struct FtCtx {
    PathBuilder* builder;
    double scaleX;
    double scaleY;
    double curX = 0.0;
    double curY = 0.0;
    bool contourOpen = false;
  };
  FtCtx ctx{&builder, ftScaleX, ftScaleY};

  funcs.move_to = [](const FT_Vector* to, void* user) -> int {
    auto* c = static_cast<FtCtx*>(user);
    if (c->contourOpen) {
      c->builder->closePath();
    }
    const double x = static_cast<double>(to->x) * c->scaleX;
    const double y = -static_cast<double>(to->y) * c->scaleY;
    c->builder->moveTo(Vector2d(x, y));
    c->curX = x;
    c->curY = y;
    c->contourOpen = true;
    return 0;
  };
  funcs.line_to = [](const FT_Vector* to, void* user) -> int {
    auto* c = static_cast<FtCtx*>(user);
    const double x = static_cast<double>(to->x) * c->scaleX;
    const double y = -static_cast<double>(to->y) * c->scaleY;
    c->builder->lineTo(Vector2d(x, y));
    c->curX = x;
    c->curY = y;
    return 0;
  };
  funcs.conic_to = [](const FT_Vector* ctrl, const FT_Vector* to, void* user) -> int {
    auto* c = static_cast<FtCtx*>(user);
    const double cx = static_cast<double>(ctrl->x) * c->scaleX;
    const double cy = -static_cast<double>(ctrl->y) * c->scaleY;
    const double ex = static_cast<double>(to->x) * c->scaleX;
    const double ey = -static_cast<double>(to->y) * c->scaleY;
    // Use native quadratic curve instead of elevating to cubic.
    c->builder->quadTo(Vector2d(cx, cy), Vector2d(ex, ey));
    c->curX = ex;
    c->curY = ey;
    return 0;
  };
  funcs.cubic_to = [](const FT_Vector* ctrl1, const FT_Vector* ctrl2, const FT_Vector* to,
                      void* user) -> int {
    auto* c = static_cast<FtCtx*>(user);
    const double ex = static_cast<double>(to->x) * c->scaleX;
    const double ey = -static_cast<double>(to->y) * c->scaleY;
    c->builder->curveTo(Vector2d(static_cast<double>(ctrl1->x) * c->scaleX,
                                 -static_cast<double>(ctrl1->y) * c->scaleY),
                        Vector2d(static_cast<double>(ctrl2->x) * c->scaleX,
                                 -static_cast<double>(ctrl2->y) * c->scaleY),
                        Vector2d(ex, ey));
    c->curX = ex;
    c->curY = ey;
    return 0;
  };

  FT_Outline_Decompose(&ftFace->glyph->outline, &funcs, &ctx);

  if (ctx.contourOpen) {
    builder.closePath();
  }

  return builder.build();
}

// ---------------------------------------------------------------------------
// Bitmap-only / bitmap glyph
// ---------------------------------------------------------------------------

bool TextBackendFull::isBitmapOnly(FontHandle font) const {
  // FreeType may decompress embedded PNG bitmap strikes while loading a glyph, before Donner can
  // inspect the decoded dimensions. Only application-controlled fonts may enter that decoder path.
  return fontManager_.isTrustedFont(font) && fontManager_.isValidatedFont(font) &&
         !HasCachedOutlineTables(fontManager_, font);
}

namespace {

/// Select a checked strike candidate without passing an absent index to FreeType.
std::optional<int> SelectBitmapStrike(FT_Face face, std::optional<int> index) {
  if (!index || FT_Select_Size(face, *index) != 0) {
    return std::nullopt;
  }
  return index;
}

/// Bitmap decoding remains restricted to trusted fonts and finite positive scales.
bool CanDecodeBitmapGlyph(const FontManager& manager, FontHandle font, int glyphIndex,
                          float scale) {
  return manager.isTrustedFont(font) && glyphIndex >= 0 && std::isfinite(scale) && scale > 0.0f;
}

std::optional<int> SelectBestBitmapStrike(FT_Face face) {
  if (face->num_fixed_sizes <= 0) {
    return -1;
  }
  constexpr int kMaximumBitmapStrikes = 256;
  if (!face->available_sizes || face->num_fixed_sizes > kMaximumBitmapStrikes) {
    return std::nullopt;
  }

  std::optional<int> bestIndex;
  FT_Short bestHeight = 0;
  constexpr FT_Pos kMaximumPpem = static_cast<FT_Pos>(details::kMaximumBitmapGlyphDimension) * 64;
  for (int i = 0; i < face->num_fixed_sizes; ++i) {
    const FT_Bitmap_Size& size = face->available_sizes[i];
    if (size.width <= 0 || size.height <= 0 ||
        size.width > static_cast<FT_Short>(details::kMaximumBitmapGlyphDimension) ||
        size.height > static_cast<FT_Short>(details::kMaximumBitmapGlyphDimension) ||
        size.x_ppem <= 0 || size.y_ppem <= 0 || size.x_ppem > kMaximumPpem ||
        size.y_ppem > kMaximumPpem) {
      continue;
    }
    if (!bestIndex || size.height > bestHeight) {
      bestHeight = size.height;
      bestIndex = i;
    }
  }
  return SelectBitmapStrike(face, bestIndex);
}

std::optional<std::size_t> LoadBitmapMetrics(FT_Face face, int glyphIndex) {
  constexpr FT_Int32 kMetricsFlags = FT_LOAD_COLOR | FT_LOAD_BITMAP_METRICS_ONLY;
  if (FT_Load_Glyph(face, static_cast<FT_UInt>(glyphIndex), kMetricsFlags) != 0 ||
      face->glyph->format != FT_GLYPH_FORMAT_BITMAP ||
      face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA) {
    return std::nullopt;
  }
  return details::ValidatedBgraOutputBytes(face->glyph->bitmap.width, face->glyph->bitmap.rows);
}

std::optional<details::BgraBitmapLayout> LoadValidatedBitmap(FT_Face face, int glyphIndex) {
  if (FT_Load_Glyph(face, static_cast<FT_UInt>(glyphIndex), FT_LOAD_COLOR) != 0 ||
      face->glyph->format != FT_GLYPH_FORMAT_BITMAP) {
    return std::nullopt;
  }
  const FT_Bitmap& bitmap = face->glyph->bitmap;
  if (bitmap.width == 0 || bitmap.rows == 0 || bitmap.pixel_mode != FT_PIXEL_MODE_BGRA) {
    return std::nullopt;
  }
  return details::ValidateBgraBitmapLayout(bitmap.width, bitmap.rows, bitmap.pitch, bitmap.buffer);
}

std::optional<double> BitmapPixelScale(hb_font_t* hbFont, FT_Face face, float requestedScale) {
  if (!face->size) {
    return std::nullopt;
  }
  const unsigned int unitsPerEm = hb_face_get_upem(hb_font_get_face(hbFont));
  const double fontSizePx = static_cast<double>(requestedScale) * static_cast<double>(unitsPerEm);
  const double strikePpem = static_cast<double>(face->size->metrics.y_ppem);
  if (!std::isfinite(fontSizePx) || fontSizePx <= 0.0 || !std::isfinite(strikePpem) ||
      strikePpem <= 0.0) {
    return std::nullopt;
  }
  return fontSizePx / strikePpem;
}

}  // namespace

std::optional<TextBackend::BitmapGlyphMetrics> TextBackendFull::bitmapGlyphMetrics(
    FontHandle font, int glyphIndex, float requestedScale) const {
  if (!CanDecodeBitmapGlyph(fontManager_, font, glyphIndex, requestedScale)) {
    return std::nullopt;
  }
  hb_font_t* hbFont = getOrCreateHbFont(font);
  const auto* entry = registry_.try_get<HbFontEntry>(font.entity());
  if (!hbFont || !entry || !entry->ftFace) {
    return std::nullopt;
  }
  FT_Face face = entry->ftFace;
  const auto strikeIndex = SelectBestBitmapStrike(face);
  if (!strikeIndex) {
    return std::nullopt;
  }
  const auto rgbaBytes = LoadBitmapMetrics(face, glyphIndex);
  const auto pixelScale = BitmapPixelScale(hbFont, face, requestedScale);
  if (!rgbaBytes || !pixelScale) {
    return std::nullopt;
  }
  return BitmapGlyphMetrics{.font = font,
                            .glyphIndex = glyphIndex,
                            .selectedStrikeIndex = *strikeIndex,
                            .width = static_cast<int>(face->glyph->bitmap.width),
                            .height = static_cast<int>(face->glyph->bitmap.rows),
                            .rgbaBytes = *rgbaBytes,
                            .bearingX = static_cast<double>(face->glyph->bitmap_left) * *pixelScale,
                            .bearingY = static_cast<double>(face->glyph->bitmap_top) * *pixelScale,
                            .scale = *pixelScale,
                            .requestedScale = requestedScale};
}

std::optional<TextBackend::BitmapGlyph> TextBackendFull::decodeBitmapGlyph(
    const BitmapGlyphMetrics& metrics) const {
  const auto* entry = registry_.try_get<HbFontEntry>(metrics.font.entity());
  if (!entry || !entry->ftFace) {
    return std::nullopt;
  }
  const auto layout = LoadValidatedBitmap(entry->ftFace, metrics.glyphIndex);
  if (!layout || layout->width != metrics.width || layout->height != metrics.height ||
      layout->rgbaBytes != metrics.rgbaBytes) {
    return std::nullopt;
  }
  BitmapGlyph result;
  result.rgbaPixels =
      details::ConvertValidatedBgraToRgba(entry->ftFace->glyph->bitmap.buffer, *layout);
  result.width = metrics.width;
  result.height = metrics.height;
  result.bearingX = metrics.bearingX;
  result.bearingY = metrics.bearingY;
  result.scale = metrics.scale;
  return result;
}

std::optional<TextBackend::BitmapGlyph> TextBackendFull::bitmapGlyph(FontHandle font,
                                                                     int glyphIndex,
                                                                     float scale) const {
  const auto metrics = bitmapGlyphMetrics(font, glyphIndex, scale);
  return metrics ? decodeBitmapGlyph(*metrics) : std::nullopt;
}

std::optional<TextBackend::BitmapGlyph> TextBackendFull::bitmapGlyph(
    FontHandle font, int glyphIndex, float scale, const BitmapGlyphMetrics& admitted,
    std::size_t maximumRgbaBytes) const {
  const auto current = bitmapGlyphMetrics(font, glyphIndex, scale);
  if (!current || *current != admitted || current->rgbaBytes > maximumRgbaBytes) {
    return std::nullopt;
  }
  return decodeBitmapGlyph(*current);
}

// ---------------------------------------------------------------------------
// Capability queries
// ---------------------------------------------------------------------------

bool TextBackendFull::isCursive(uint32_t codepoint) const {
  return isCursiveScript(codepoint);
}

bool TextBackendFull::hasSmallCapsFeature(FontHandle font) const {
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return false;
  }

  hb_face_t* hbFace = hb_font_get_face(hbFont);
  if (!hbFace) {
    return false;
  }

  unsigned int featureIndex = 0;
  return hb_ot_layout_language_find_feature(hbFace, HB_OT_TAG_GSUB, 0,
                                            HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX,
                                            HB_TAG('s', 'm', 'c', 'p'), &featureIndex);
}

// ---------------------------------------------------------------------------
// Shaping
// ---------------------------------------------------------------------------

TextBackend::ShapedRun TextBackendFull::shapeRun(FontHandle font, float fontSizePx,
                                                 std::string_view spanText, size_t byteOffset,
                                                 size_t byteLength, bool isVertical,
                                                 FontVariant fontVariant,
                                                 bool forceLogicalOrder) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      TextOrientation::Mixed, fontVariant, true, forceLogicalOrder);
}

TextBackend::ShapedRun TextBackendFull::shapeRun(FontHandle font, float fontSizePx,
                                                 std::string_view spanText, size_t byteOffset,
                                                 size_t byteLength, bool isVertical,
                                                 TextOrientation textOrientation,
                                                 FontVariant fontVariant,
                                                 bool forceLogicalOrder) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      textOrientation, fontVariant, true, forceLogicalOrder);
}

TextBackend::ShapedRun TextBackendFull::shapeRunNoKerning(FontHandle font, float fontSizePx,
                                                          std::string_view spanText,
                                                          size_t byteOffset, size_t byteLength,
                                                          bool isVertical, FontVariant fontVariant,
                                                          bool forceLogicalOrder) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      TextOrientation::Mixed, fontVariant, false, forceLogicalOrder);
}

TextBackend::ShapedRun TextBackendFull::shapeRunNoKerning(
    FontHandle font, float fontSizePx, std::string_view spanText, size_t byteOffset,
    size_t byteLength, bool isVertical, TextOrientation textOrientation, FontVariant fontVariant,
    bool forceLogicalOrder) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      textOrientation, fontVariant, false, forceLogicalOrder);
}

TextBackend::ShapedRun TextBackendFull::shapeRunInContext(
    FontHandle font, float fontSizePx, std::string_view spanText, const ShapingContext& context,
    size_t byteOffset, size_t byteLength, bool isVertical, TextOrientation textOrientation,
    FontVariant fontVariant, bool kerning) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      textOrientation, fontVariant, kerning, false, &context);
}

namespace {

struct RunScriptProperties {
  bool vertical;
  hb_direction_t direction;
  hb_script_t script;
  hb_language_t language;
};

/// Detects the direction, script, and language used by every subrange of one run.
RunScriptProperties DetectRunScript(const char* chunkData, size_t byteLength, bool isVertical,
                                    TextOrientation textOrientation) {
  const int chunkLen = static_cast<int>(byteLength);
  bool useVerticalShaping = false;
  if (isVertical && textOrientation == TextOrientation::Upright) {
    useVerticalShaping = true;
  } else if (isVertical && textOrientation == TextOrientation::Mixed) {
    for (size_t i = 0; i < byteLength;) {
      const uint32_t cp = decodeCodepointAt(std::string_view(chunkData, byteLength), i);
      if (cp >= 0x2E80) {
        useVerticalShaping = true;
        break;
      }

      const auto byte = static_cast<uint8_t>(chunkData[i]);
      if (byte >= 0xF0) {
        i += 4;
      } else if (byte >= 0xE0) {
        i += 3;
      } else if (byte >= 0xC0) {
        i += 2;
      } else {
        i += 1;
      }
    }
  }

  hb_buffer_t* detectBuf = hb_buffer_create();
  hb_buffer_add_utf8(detectBuf, chunkData, chunkLen, 0, chunkLen);
  if (useVerticalShaping) {
    hb_buffer_set_direction(detectBuf, HB_DIRECTION_TTB);
  }
  hb_buffer_guess_segment_properties(detectBuf);
  const hb_direction_t detectedDirection = hb_buffer_get_direction(detectBuf);
  const hb_script_t detectedScript = hb_buffer_get_script(detectBuf);
  const hb_language_t detectedLanguage = hb_buffer_get_language(detectBuf);
  hb_buffer_destroy(detectBuf);

  return {useVerticalShaping, detectedDirection, detectedScript, detectedLanguage};
}

/// Detects the context's shaping properties on first use and reuses them for its other ranges.
RunScriptProperties DetectContextScript(std::string_view spanText,
                                        const TextBackend::ShapingContext& context, bool isVertical,
                                        TextOrientation textOrientation,
                                        std::size_t& detectedContextBytes) {
  auto& detected = context.detected;
  if (!detected.valid) {
    detectedContextBytes += context.byteLength;
    const RunScriptProperties properties = DetectRunScript(
        spanText.data() + context.byteOffset, context.byteLength, isVertical, textOrientation);
    detected = {true, properties.vertical, static_cast<uint32_t>(properties.direction),
                static_cast<uint32_t>(properties.script), properties.language};
  }
  return {detected.vertical, static_cast<hb_direction_t>(detected.direction),
          static_cast<hb_script_t>(detected.script), static_cast<hb_language_t>(detected.language)};
}

/// Whether \p text contains a decimal digit.
bool ContainsDecimalDigit(std::string_view text) {
  hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
  for (size_t position = 0; position < text.size();) {
    const auto [codepoint, length] = Utf8::NextCodepoint(text.substr(position));
    position += static_cast<size_t>(std::max(length, 1));
    if (hb_unicode_general_category(unicode, static_cast<hb_codepoint_t>(codepoint)) ==
        HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER) {
      return true;
    }
  }
  return false;
}

/**
 * Detects the shaping properties of one range of a context, by the rules documented on
 * \ref TextBackend::shapeRunInContext.
 */
RunScriptProperties DetectRangeScriptInContext(std::string_view spanText,
                                               const TextBackend::ShapingContext& context,
                                               size_t byteOffset, size_t byteLength,
                                               bool isVertical, TextOrientation textOrientation,
                                               std::size_t& detectedContextBytes) {
  const RunScriptProperties contextProperties =
      DetectContextScript(spanText, context, isVertical, textOrientation, detectedContextBytes);
  RunScriptProperties range =
      DetectRunScript(spanText.data() + byteOffset, byteLength, isVertical, textOrientation);
  // Guessing leaves the script invalid when every character is common or inherited.
  if (range.script == HB_SCRIPT_INVALID || range.script == HB_SCRIPT_COMMON ||
      range.script == HB_SCRIPT_INHERITED || range.script == HB_SCRIPT_UNKNOWN) {
    RunScriptProperties neutral = contextProperties;
    if (!neutral.vertical && ContainsDecimalDigit(spanText.substr(byteOffset, byteLength))) {
      neutral.direction = HB_DIRECTION_LTR;
    }
    return neutral;
  }
  if (contextProperties.vertical && !range.vertical && range.direction == HB_DIRECTION_LTR) {
    range.vertical = true;
    range.direction = HB_DIRECTION_TTB;
  }
  return range;
}

/// Detects the shaping properties of a range, in its context when it has one.
RunScriptProperties DetectShapingScript(std::string_view spanText,
                                        const TextBackend::ShapingContext* context,
                                        size_t byteOffset, size_t byteLength, bool isVertical,
                                        TextOrientation textOrientation,
                                        std::size_t& detectedContextBytes) {
  if (context) {
    return DetectRangeScriptInContext(spanText, *context, byteOffset, byteLength, isVertical,
                                      textOrientation, detectedContextBytes);
  }
  return DetectRunScript(spanText.data() + byteOffset, byteLength, isVertical, textOrientation);
}

/**
 * The direction to shape one buffer with. A horizontal single-codepoint buffer without context is
 * shaped left to right regardless of its script: when per-character coordinates split right-to-left
 * text into individual characters, each is shaped independently to avoid right-to-left GPOS
 * adjustments that differ from the connected-text advances. A range shaped in context keeps its
 * detected direction.
 */
hb_direction_t ShapingDirection(hb_direction_t detected, bool verticalShaping,
                                unsigned int bufferLength, bool inContext) {
  if (verticalShaping || bufferLength > 1 || inContext) {
    return detected;
  }
  return HB_DIRECTION_LTR;
}

bool ContextAddsShapingInformation(const TextBackend::ShapingContext& context, size_t rangeLength) {
  return context.byteLength > rangeLength || context.rightToLeft.has_value();
}

hb_direction_t ResolvedShapingDirection(const TextBackend::ShapingContext* context,
                                        hb_direction_t detected, bool vertical,
                                        unsigned int bufferLength, bool inContext) {
  if (context && context->rightToLeft && !vertical) {
    return *context->rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;
  }
  return ShapingDirection(detected, vertical, bufferLength, inContext);
}

/**
 * Adds [\p rangeStart, \p rangeEnd) of \p text, which starts at \p byteOffset of \p spanText, to
 * \p buf. With a context, the context's other characters become the buffer's pre- and
 * post-context.
 *
 * @return The value to add to the buffer's cluster values to make them relative to \p text.
 */
int64_t AddShapingRange(hb_buffer_t* buf, const char* text, size_t rangeStart, size_t rangeEnd,
                        std::string_view spanText, size_t byteOffset,
                        const TextBackend::ShapingContext* context) {
  const int length = static_cast<int>(rangeEnd - rangeStart);
  if (!context) {
    hb_buffer_add_utf8(buf, text + rangeStart, length, 0, length);
    return static_cast<int64_t>(rangeStart);
  }
  const size_t rangeInContext = byteOffset - context->byteOffset;
  hb_buffer_add_utf8(buf, spanText.data() + context->byteOffset,
                     static_cast<int>(context->byteLength),
                     static_cast<unsigned int>(rangeInContext + rangeStart), length);
  return -static_cast<int64_t>(rangeInContext);
}

struct SmallCapsPreparation {
  bool useFeature;
  std::string text;
  std::vector<bool> smallCapBytes;
};

/// Selects native small caps or prepares the existing ASCII synthesis and byte flags.
SmallCapsPreparation PrepareSmallCaps(hb_font_t* hbFont, const char* chunkData, size_t byteLength,
                                      FontVariant fontVariant) {
  // Small-caps handling.
  bool useSmcpFeature = false;
  std::string smallCapsText;
  std::vector<bool> isSmallCap;

  if (fontVariant == FontVariant::SmallCaps) {
    // Check for native OpenType small-caps feature.
    hb_face_t* hbFace = hb_font_get_face(hbFont);
    unsigned int featureIndex = 0;
    useSmcpFeature = hbFace && hb_ot_layout_language_find_feature(
                                   hbFace, HB_OT_TAG_GSUB, 0, HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX,
                                   HB_TAG('s', 'm', 'c', 'p'), &featureIndex);

    if (!useSmcpFeature) {
      // Synthesize: convert lowercase to uppercase and track which bytes changed.
      const std::string_view chunk(chunkData, byteLength);
      smallCapsText.reserve(byteLength);
      isSmallCap.resize(byteLength, false);
      for (size_t i = 0; i < byteLength; ++i) {
        char ch = chunk[i];
        if (ch >= 'a' && ch <= 'z') {
          smallCapsText.push_back(ch - 'a' + 'A');
          isSmallCap[i] = true;
        } else {
          smallCapsText.push_back(ch);
        }
      }
    }
  }

  return {useSmcpFeature, std::move(smallCapsText), std::move(isSmallCap)};
}

}  // namespace

namespace {

struct ShapedGlyphInfo {
  hb_glyph_info_t info;
  hb_glyph_position_t pos;
  bool isSynthSmallCap;
};

bool UsesSidewaysMetrics(TextOrientation orientation, uint32_t codepoint) {
  return orientation == TextOrientation::Sideways ||
         (orientation == TextOrientation::Mixed && codepoint > 0 && codepoint < 0x2E80);
}

/// Converts sideways glyph metrics while retaining upright vertical shaping offsets.
void AdjustVerticalGlyph(TextBackend::ShapedGlyph& glyph, const ShapedGlyphInfo& sg,
                         hb_font_t* hbFont, double pixelScaleX, std::string_view spanText,
                         TextOrientation textOrientation) {
  const uint32_t codepoint = decodeCodepointAt(spanText, glyph.cluster);
  const bool sideways = UsesSidewaysMetrics(textOrientation, codepoint);
  if (sideways) {
    glyph.xAdvance =
        static_cast<double>(hb_font_get_glyph_h_advance(hbFont, sg.info.codepoint)) * pixelScaleX;
    glyph.yAdvance = 0.0;
    glyph.xOffset = 0.0;
    glyph.yOffset = 0.0;
  }
}

/// Converts font-scaled shaping output to document-pixel advances, offsets, and clusters.
TextBackend::ShapedRun ConvertShapedRun(const std::vector<ShapedGlyphInfo>& allGlyphs,
                                        hb_font_t* hbFont, double pixelScaleX, double pixelScaleY,
                                        std::string_view spanText, size_t byteOffset,
                                        bool isVertical, bool useVerticalShaping,
                                        TextOrientation textOrientation, bool forceLogicalOrder) {
  // Convert shaped glyphs to ShapedRun output.
  TextBackend::ShapedRun result;
  result.glyphs.reserve(allGlyphs.size());

  for (const auto& sg : allGlyphs) {
    TextBackend::ShapedGlyph glyph;
    glyph.glyphIndex = static_cast<int>(sg.info.codepoint);
    glyph.xAdvance = static_cast<double>(sg.pos.x_advance) * pixelScaleX;
    glyph.yAdvance = std::abs(static_cast<double>(sg.pos.y_advance) * pixelScaleY);
    glyph.xOffset = static_cast<double>(sg.pos.x_offset) * pixelScaleX;
    glyph.yOffset = -static_cast<double>(sg.pos.y_offset) * pixelScaleY;
    glyph.xKern = 0;  // Kerning is baked into advances by GPOS.
    glyph.yKern = 0;
    glyph.cluster = sg.info.cluster + static_cast<uint32_t>(byteOffset);
    glyph.fontSizeScale = sg.isSynthSmallCap ? kSmallCapScale : 1.0f;

    if (isVertical && useVerticalShaping) {
      AdjustVerticalGlyph(glyph, sg, hbFont, pixelScaleX, spanText, textOrientation);
    }

    result.glyphs.push_back(glyph);
  }

  // When forceLogicalOrder is set, sort glyphs by cluster (DOM order).
  // HarfBuzz outputs RTL glyphs in visual order (reversed). Sorting to DOM order
  // allows the engine to process per-character positioning in source text order.
  if (forceLogicalOrder && result.glyphs.size() > 1) {
    std::sort(result.glyphs.begin(), result.glyphs.end(),
              [](const TextBackend::ShapedGlyph& a, const TextBackend::ShapedGlyph& b) {
                return a.cluster < b.cluster;
              });
  }

  return result;
}

}  // namespace

namespace {

/// Advances over a single UTF-8 codepoint using the shaping loop's existing byte classification.
size_t NextShapingByte(const char* text, size_t offset) {
  const auto byte = static_cast<uint8_t>(text[offset]);
  if (byte >= 0xF0) {
    return offset + 4;
  }
  if (byte >= 0xE0) {
    return offset + 3;
  }
  if (byte >= 0xC0) {
    return offset + 2;
  }
  return offset + 1;
}

/// Shapes native small caps or the existing synthesized subranges at their original sizes.
template <typename ShapeRange>
bool ShapeVariantRanges(FT_Face ftFace, hb_font_t* hbFont, float fontSizePx, const char* chunkData,
                        size_t byteLength, const SmallCapsPreparation& smallCaps,
                        hb_feature_t* features, unsigned int numFeatures, ShapeRange&& shapeRange) {
  const bool useSmcpFeature = smallCaps.useFeature;
  const auto& isSmallCap = smallCaps.smallCapBytes;
  const char* shapeText = smallCaps.text.empty() ? chunkData : smallCaps.text.data();
  if (useSmcpFeature) {
    shapeRange(chunkData, 0, byteLength, false, features, numFeatures);
  } else if (!isSmallCap.empty()) {
    // Synthesized small-caps: split into sub-runs at small-cap boundaries
    // and shape each at the appropriate font size.
    size_t bi = 0;
    while (bi < byteLength) {
      const bool sc = bi < isSmallCap.size() && isSmallCap[bi];
      const size_t subStart = bi;
      while (bi < byteLength && (bi < isSmallCap.size() && isSmallCap[bi]) == sc) {
        bi = NextShapingByte(shapeText, bi);
      }

      if (sc) {
        // Shape small-cap sub-run at reduced font size.
        if (!SetScalableFaceSize(hbFont, ftFace, fontSizePx * kSmallCapScale)) {
          return false;
        }

        shapeRange(shapeText, subStart, bi, true, features, numFeatures);

        // Restore full font size.
        if (!SetScalableFaceSize(hbFont, ftFace, fontSizePx)) {
          return false;
        }
      } else {
        shapeRange(shapeText, subStart, bi, false, features, numFeatures);
      }
    }
  } else {
    shapeRange(chunkData, 0, byteLength, false, features, numFeatures);
  }
  return true;
}

}  // namespace

namespace {

/// Synchronous callback state borrowed only by a range-local HarfBuzz child font.
struct VerticalMetricAdmission {
  const FontManager& manager;
  FontHandle handle;
  TextBackend::ShapingContext::WorkBudget& work;
  std::size_t& originCalls;
  std::size_t& advanceCalls;

  bool admit(hb_codepoint_t glyph) {
    if (work.rejected) {
      return false;
    }
    const auto complexity = glyph <= static_cast<hb_codepoint_t>(std::numeric_limits<int>::max())
                                ? manager.glyphOutlineComplexity(handle, static_cast<int>(glyph))
                                : std::nullopt;
    if (!complexity || complexity->maximumVertices > work.glyphOutlineVertices ||
        complexity->work > work.glyphOutlineWork) {
      work.rejected = true;
      return false;
    }
    work.glyphOutlineVertices -= complexity->maximumVertices;
    work.glyphOutlineWork -= complexity->work;
    return true;
  }
};

hb_bool_t BoundedVerticalOrigin(hb_font_t* font, void* data, hb_codepoint_t glyph, hb_position_t* x,
                                hb_position_t* y, void*) {
  auto& admission = *static_cast<VerticalMetricAdmission*>(data);
  *x = *y = 0;
  if (admission.admit(glyph)) {
    ++admission.originCalls;
    if (!hb_font_get_glyph_v_origin(hb_font_get_parent(font), glyph, x, y)) {
      admission.work.rejected = true;
      *x = *y = 0;
    }
  }
  // Success suppresses HarfBuzz's outline-extents fallback; the entire result is rejected later.
  return true;
}

hb_position_t BoundedVerticalAdvance(hb_font_t* font, void* data, hb_codepoint_t glyph, void*) {
  auto& admission = *static_cast<VerticalMetricAdmission*>(data);
  if (!admission.admit(glyph)) {
    return 0;
  }
  ++admission.advanceCalls;
  return hb_font_get_glyph_v_advance(hb_font_get_parent(font), glyph);
}

using HbFontOwner = std::unique_ptr<hb_font_t, decltype(&hb_font_destroy)>;

/// Inherits the parent's current face, scale and ppem after each small-cap size change.
HbFontOwner MakeVerticalMetricFont(hb_font_t* parent, VerticalMetricAdmission& admission) {
  HbFontOwner child(hb_font_create_sub_font(parent), &hb_font_destroy);
  hb_font_funcs_t* functions = hb_font_funcs_create();
  if (hb_font_is_immutable(child.get()) || hb_font_funcs_is_immutable(functions)) {
    admission.work.rejected = true;
    hb_font_funcs_destroy(functions);
    return HbFontOwner(nullptr, &hb_font_destroy);
  }
  hb_font_funcs_set_glyph_v_origin_func(functions, &BoundedVerticalOrigin, nullptr, nullptr);
  hb_font_funcs_set_glyph_v_advance_func(functions, &BoundedVerticalAdvance, nullptr, nullptr);
  hb_font_funcs_make_immutable(functions);
  hb_font_set_funcs(child.get(), functions, &admission, nullptr);
  hb_font_funcs_destroy(functions);
  return child;
}

/// The selected budget aliases the caller's context or fallback through synchronous shaping.
TextBackend::ShapingContext::WorkBudget& SelectMetricWork(
    const TextBackend::ShapingContext* context, TextBackend::ShapingContext::WorkBudget& fallback) {
  return context && context->work ? *context->work : fallback;
}

/// Shapes through the bounded child only for untrusted vertical fonts.
bool ShapeWithMetricAdmission(hb_font_t* parent, hb_buffer_t* buffer, const hb_feature_t* features,
                              unsigned int numFeatures, VerticalMetricAdmission& admission) {
  if (admission.work.rejected) {
    return false;
  }
  HbFontOwner metricFont(nullptr, &hb_font_destroy);
  if (hb_buffer_get_direction(buffer) == HB_DIRECTION_TTB &&
      !admission.manager.isTrustedFont(admission.handle)) {
    metricFont = MakeVerticalMetricFont(parent, admission);
  }
  if (admission.work.rejected) {
    return false;
  }
  hb_shape(metricFont ? metricFont.get() : parent, buffer, features, numFeatures);
  return !admission.work.rejected;
}

/// Returns the first advance of a bounded pair/solo probe without retaining callback state.
std::optional<double> ShapeFirstAdvance(hb_font_t* font, std::span<const uint32_t> codepoints,
                                        hb_direction_t direction, double pixelScaleX,
                                        double pixelScaleY, VerticalMetricAdmission& admission,
                                        const TextBackend::ShapingContext* context = nullptr,
                                        bool enableKerning = true) {
  hb_buffer_t* buffer = hb_buffer_create();
  hb_buffer_add_codepoints(buffer, codepoints.data(), codepoints.size(), 0, codepoints.size());
  hb_buffer_set_direction(buffer, direction);
  hb_buffer_set_script(buffer, context ? HB_SCRIPT_INVALID : HB_SCRIPT_LATIN);
  hb_buffer_guess_segment_properties(buffer);
  const hb_feature_t withoutKerning{HB_TAG('k', 'e', 'r', 'n'), 0, 0, UINT_MAX};
  if (!ShapeWithMetricAdmission(font, buffer, enableKerning ? nullptr : &withoutKerning,
                                enableKerning ? 0 : 1, admission)) {
    hb_buffer_destroy(buffer);
    return std::nullopt;
  }
  unsigned int count = 0;
  const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
  std::optional<double> advance;
  if (count != 0) {
    advance = direction == HB_DIRECTION_TTB
                  ? std::abs(static_cast<double>(positions[0].y_advance) * pixelScaleY)
                  : static_cast<double>(positions[0].x_advance) * pixelScaleX;
  }
  hb_buffer_destroy(buffer);
  return advance;
}

hb_direction_t PairShapingDirection(const TextBackend::ShapingContext* context, bool vertical,
                                    TextOrientation orientation, uint32_t codepoint) {
  const bool upright = vertical && !UsesSidewaysMetrics(orientation, codepoint);
  const auto fallback = upright ? HB_DIRECTION_TTB : HB_DIRECTION_LTR;
  return ResolvedShapingDirection(context, fallback, upright, 2, context != nullptr);
}

std::array<uint32_t, 2> LogicalPair(uint32_t previous, uint32_t current, hb_direction_t direction) {
  return direction == HB_DIRECTION_RTL ? std::array{current, previous}
                                       : std::array{previous, current};
}

/// The same substitutions in both probes isolate positioning from already-contextual joining.
double ContextPairAdjustment(hb_font_t* font, std::span<const uint32_t> pair,
                             hb_direction_t direction, double scaleX, double scaleY,
                             VerticalMetricAdmission& admission,
                             const TextBackend::ShapingContext& context) {
  if (context.work && !context.work->consume(pair.size() * 2)) {
    return 0;
  }
  const auto enabled =
      ShapeFirstAdvance(font, pair, direction, scaleX, scaleY, admission, &context, true);
  const auto disabled =
      ShapeFirstAdvance(font, pair, direction, scaleX, scaleY, admission, &context, false);
  return enabled && disabled ? *enabled - *disabled : 0;
}

bool ValidKerningFaces(const FontManager& manager, FontHandle previous, FontHandle current,
                       float currentSize) {
  return manager.fontsShareFamily(previous, current) && std::isfinite(currentSize) &&
         currentSize > 0;
}

bool AdmitContextualShaping(const TextBackend::ShapingContext* context, size_t bytes) {
  return !context || !context->work ||
         context->work->consume(bytes, context->detected.valid ? 0 : context->byteLength);
}

struct ShapingOrder {
  std::optional<bool> backward;
  bool mixed = false;

  void observe(hb_direction_t direction) {
    const bool value = HB_DIRECTION_IS_BACKWARD(direction);
    mixed = mixed || (backward && *backward != value);
    backward = value;
  }

  std::optional<bool> output(bool logical) const {
    return logical ? std::optional<bool>(false) : (mixed ? std::nullopt : backward);
  }
};

struct ShapingFeatures {
  std::array<hb_feature_t, 8> values;
  unsigned int count = 0;
};

ShapingFeatures MakeShapingFeatures(bool smallCaps, bool kerning,
                                    const TextBackend::ShapingContext* context) {
  ShapingFeatures result;
  if (smallCaps) {
    result.values[result.count++] = {HB_TAG('s', 'm', 'c', 'p'), 1, 0, UINT_MAX};
  }
  if (!kerning) {
    result.values[result.count++] = {HB_TAG('k', 'e', 'r', 'n'), 0, 0, UINT_MAX};
  }
  if (context && context->suppressOptionalSubstitutions) {
    constexpr hb_tag_t tags[] = {HB_TAG('l', 'i', 'g', 'a'), HB_TAG('c', 'l', 'i', 'g'),
                                 HB_TAG('c', 'a', 'l', 't'), HB_TAG('d', 'l', 'i', 'g'),
                                 HB_TAG('h', 'l', 'i', 'g')};
    for (hb_tag_t tag : tags) {
      result.values[result.count++] = {tag, 0, 0, UINT_MAX};
    }
  }
  if (context && context->suppressRligForAnalysis) {
    result.values[result.count++] = {HB_TAG('r', 'l', 'i', 'g'), 0, 0, UINT_MAX};
  }
  return result;
}

}  // namespace

TextBackend::ShapedRun TextBackendFull::shapeRunImpl(
    FontHandle font, float fontSizePx, std::string_view spanText, size_t byteOffset,
    size_t byteLength, bool isVertical, TextOrientation textOrientation, FontVariant fontVariant,
    bool enableKerning, bool forceLogicalOrder, const ShapingContext* context) const {
  if (!AdmitContextualShaping(context, byteLength)) {
    return {};
  }
  TextBackend::ShapingContext::WorkBudget localMetricWork{0, 0};
  auto& metricWork = SelectMetricWork(context, localMetricWork);
  VerticalMetricAdmission metricAdmission{fontManager_, font, metricWork,
                                          delegatedVerticalOriginCalls_,
                                          delegatedVerticalAdvanceCalls_};
  shapedBytes_ += byteLength;
  const ShapingContext* const featureContext = context;
  // A context that adds no characters leaves the range shaped as without one.
  if (context && !ContextAddsShapingInformation(*context, byteLength)) {
    context = nullptr;
  }
  hb_font_t* hbFont = getOrCreateHbFont(font);
  if (!hbFont) {
    return {};
  }

  FT_Face ftFace = hb_ft_font_get_ft_face(hbFont);
  if (!PrepareShapingSize(hbFont, ftFace, fontSizePx)) {
    return {};
  }

  // With FreeType-backed fonts, HarfBuzz returns positions in 26.6 fixed-point pixels
  // at the face's current size. For scalable fonts that's fontSizePx; for bitmap fonts
  // it's the strike's ppem. Scale accordingly to get document-pixel coordinates.
  const double pixelScaleX = pixelScaleForPpem(ftFace, fontSizePx, true);
  const double pixelScaleY = pixelScaleForPpem(ftFace, fontSizePx, false);

  // Detect direction and script from the text chunk.
  const char* chunkData = spanText.data() + byteOffset;
  const auto scriptProperties =
      DetectShapingScript(spanText, context, byteOffset, byteLength, isVertical, textOrientation,
                          detectedContextBytes_);
  const bool useVerticalShaping = scriptProperties.vertical;
  const auto detectedDirection = scriptProperties.direction;
  const auto detectedScript = scriptProperties.script;
  const auto detectedLanguage = scriptProperties.language;

  const auto smallCaps = PrepareSmallCaps(hbFont, chunkData, byteLength, fontVariant);
  const bool useSmcpFeature = smallCaps.useFeature;
  // Synthesized small caps shape an uppercased copy of the range, which has no context.
  const ShapingContext* const shapingContext = smallCaps.text.empty() ? context : nullptr;

  // Lambda to shape a byte range and collect results.
  // rangeStart/rangeEnd are byte offsets relative to the start of `text`.
  std::vector<ShapedGlyphInfo> allGlyphs;
  ShapingOrder order;

  auto shapeRange = [&](const char* text, size_t rangeStart, size_t rangeEnd, bool isSmallCapRange,
                        hb_feature_t* features, unsigned int numFeatures) {
    hb_buffer_t* buf = hb_buffer_create();
    const int64_t clusterOffset =
        AddShapingRange(buf, text, rangeStart, rangeEnd, spanText, byteOffset, shapingContext);

    const auto direction =
        ResolvedShapingDirection(featureContext, detectedDirection, useVerticalShaping,
                                 hb_buffer_get_length(buf), shapingContext != nullptr);
    order.observe(direction);
    hb_buffer_set_direction(buf, direction);
    hb_buffer_set_script(buf, detectedScript);
    hb_buffer_set_language(buf, detectedLanguage);
    if (!ShapeWithMetricAdmission(hbFont, buf, features, numFeatures, metricAdmission)) {
      hb_buffer_destroy(buf);
      return;
    }

    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buf, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buf, &count);
    for (unsigned int i = 0; i < count; ++i) {
      hb_glyph_info_t info = infos[i];
      info.cluster = static_cast<unsigned int>(static_cast<int64_t>(info.cluster) + clusterOffset);
      allGlyphs.push_back({info, positions[i], isSmallCapRange});
    }
    hb_buffer_destroy(buf);
  };

  auto features = MakeShapingFeatures(useSmcpFeature, enableKerning, featureContext);

  if (!ShapeVariantRanges(ftFace, hbFont, fontSizePx, chunkData, byteLength, smallCaps,
                          features.values.data(), features.count, shapeRange)) {
    return {};
  }

  if (metricWork.rejected) {
    return {};
  }

  auto result =
      ConvertShapedRun(allGlyphs, hbFont, pixelScaleX, pixelScaleY, spanText, byteOffset,
                       isVertical, useVerticalShaping, textOrientation, forceLogicalOrder);
  result.backward = order.output(forceLogicalOrder);
  return result;
}

// ---------------------------------------------------------------------------
// Cross-span kerning
// ---------------------------------------------------------------------------

double TextBackendFull::crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle curFont,
                                      float curSizePx, uint32_t prevCodepoint,
                                      uint32_t curCodepoint, bool isVertical) const {
  return crossSpanKern(prevFont, prevSizePx, curFont, curSizePx, prevCodepoint, curCodepoint,
                       isVertical, TextOrientation::Mixed);
}

double TextBackendFull::crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle curFont,
                                      float curSizePx, uint32_t prevCodepoint,
                                      uint32_t curCodepoint, bool isVertical,
                                      TextOrientation textOrientation) const {
  return crossSpanKernImpl(prevFont, prevSizePx, curFont, curSizePx, prevCodepoint, curCodepoint,
                           isVertical, textOrientation, nullptr);
}

double TextBackendFull::crossSpanKern(FontHandle previousFont, float previousSize,
                                      FontHandle currentFont, float currentSize,
                                      uint32_t previousCodepoint, uint32_t currentCodepoint,
                                      bool vertical, TextOrientation orientation,
                                      const ShapingContext& context) const {
  return crossSpanKernImpl(previousFont, previousSize, currentFont, currentSize, previousCodepoint,
                           currentCodepoint, vertical, orientation, &context);
}

double TextBackendFull::crossSpanKernImpl(FontHandle prevFont, float prevSizePx, FontHandle curFont,
                                          float curSizePx, uint32_t prevCodepoint,
                                          uint32_t curCodepoint, bool isVertical,
                                          TextOrientation textOrientation,
                                          const ShapingContext* context) const {
  if (!ValidKerningFaces(fontManager_, prevFont, curFont, curSizePx)) {
    return 0.0;
  }
  hb_font_t* hbFont = getOrCreateHbFont(prevFont);
  if (!hbFont) {
    return 0.0;
  }

  FT_Face ftFace = hb_ft_font_get_ft_face(hbFont);
  if (!PrepareShapingSize(hbFont, ftFace, prevSizePx)) {
    return 0.0;
  }

  const double pixelScaleX = pixelScaleForPpem(ftFace, prevSizePx, true);
  const double pixelScaleY = pixelScaleForPpem(ftFace, prevSizePx, false);

  TextBackend::ShapingContext::WorkBudget localWork{0, 0};
  auto& metricWork = SelectMetricWork(context, localWork);
  VerticalMetricAdmission metricAdmission{fontManager_, prevFont, metricWork,
                                          delegatedVerticalOriginCalls_,
                                          delegatedVerticalAdvanceCalls_};

  const auto direction = PairShapingDirection(context, isVertical, textOrientation, curCodepoint);
  const auto pair = LogicalPair(prevCodepoint, curCodepoint, direction);
  if (context) {
    return ContextPairAdjustment(hbFont, pair, direction, pixelScaleX, pixelScaleY, metricAdmission,
                                 *context);
  }
  const auto paired =
      ShapeFirstAdvance(hbFont, pair, direction, pixelScaleX, pixelScaleY, metricAdmission);
  if (!paired) {
    return 0.0;
  }
  const std::array solo{prevCodepoint};
  const auto standalone =
      ShapeFirstAdvance(hbFont, solo, direction, pixelScaleX, pixelScaleY, metricAdmission);
  return standalone ? *paired - *standalone : 0.0;
}

}  // namespace donner::svg
