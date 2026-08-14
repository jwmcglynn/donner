#include "donner/svg/text/TextBackendSimple.h"

#include "donner/base/Utf8.h"
#define STBTT_DEF extern
#include <stb/stb_truetype.h>

namespace donner::svg {

namespace {
int16_t ReadInt16Be(std::span<const uint8_t> data, size_t offset) {
  return static_cast<int16_t>(
      static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]));
}

/// Decode one UTF-8 codepoint from \p str starting at \p i. Advances \p i past the codepoint.
uint32_t decodeUtf8(std::string_view str, size_t& i) {
  const auto [cp, length] = Utf8::NextCodepoint(str.substr(i));
  i += static_cast<size_t>(length);
  return static_cast<uint32_t>(cp);
}

constexpr float kSmallCapScale = 0.8f;

bool HasCachedOutlineTables(const FontManager& fontManager, FontHandle font) {
  return fontManager.sfntTable(font, "glyf").has_value() ||
         fontManager.sfntTable(font, "CFF ").has_value() ||
         fontManager.sfntTable(font, "CFF2").has_value();
}

bool HasCachedCffTables(const FontManager& fontManager, FontHandle font) {
  return fontManager.sfntTable(font, "CFF ").has_value() ||
         fontManager.sfntTable(font, "CFF2").has_value();
}

/// Cached stb_truetype parse state attached to a font entity.
struct StbFontComponent {
  stbtt_fontinfo fontInfo{};
  bool valid = false;
};

}  // namespace

TextBackendSimple::TextBackendSimple(FontManager& fontManager, Registry& registry)
    : fontManager_(fontManager), registry_(registry) {}

const stbtt_fontinfo* TextBackendSimple::getFontInfo(FontHandle font) const {
  if (!font) {
    return nullptr;
  }

  if (const auto* cached = registry_.try_get<StbFontComponent>(font.entity())) {
    return cached->valid ? &cached->fontInfo : nullptr;
  }

  if (!fontManager_.isValidatedFont(font)) {
    return nullptr;
  }
  auto& cached = registry_.emplace<StbFontComponent>(font.entity());

  // stb_truetype gives its CFF parser a synthetic 512 MiB span instead of the directory's exact
  // table length. Refuse any CFF/CFF2-bearing font before passing its bytes to stb; the text-full
  // backend may still use FreeType, whose face constructor receives the exact font span.
  if (HasCachedCffTables(fontManager_, font) || !fontManager_.sfntTable(font, "glyf").has_value()) {
    return nullptr;
  }

  const auto fontData = fontManager_.fontData(font);
  if (stbtt_InitFont(&cached.fontInfo, fontData.data(), 0)) {
    cached.valid = true;
  }

  return cached.valid ? &cached.fontInfo : nullptr;
}

FontVMetrics TextBackendSimple::fontVMetrics(FontHandle font) const {
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return {};
  }
  FontVMetrics metrics;
  stbtt_GetFontVMetrics(info, &metrics.ascent, &metrics.descent, &metrics.lineGap);

  // x-height from the OS/2 table (`sxHeight`, offset 86), present in version >= 2.
  if (const auto os2 = fontManager_.sfntTable(font, "OS/2"); os2 && os2->size() >= 88) {
    const uint16_t version = static_cast<uint16_t>(ReadInt16Be(*os2, 0));
    if (version >= 2) {
      metrics.xHeight = ReadInt16Be(*os2, 86);
    }
  }
  return metrics;
}

float TextBackendSimple::scaleForPixelHeight(FontHandle font, float pixelHeight) const {
  const stbtt_fontinfo* info = getFontInfo(font);
  if (info) {
    return stbtt_ScaleForMappingEmToPixels(info, pixelHeight);
  }

  const auto head = fontManager_.sfntTable(font, "head");
  const uint16_t upem =
      head && head->size() >= 20 ? static_cast<uint16_t>(((*head)[18] << 8) | (*head)[19]) : 0;
  return upem > 0 ? pixelHeight / static_cast<float>(upem) : 0.0f;
}

float TextBackendSimple::scaleForEmToPixels(FontHandle font, float pixelHeight) const {
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return 0.0f;
  }
  return stbtt_ScaleForMappingEmToPixels(info, pixelHeight);
}

std::optional<UnderlineMetrics> TextBackendSimple::underlineMetrics(FontHandle font) const {
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return std::nullopt;
  }

  const auto table = fontManager_.sfntTable(font, "post");
  if (!table || table->size() < 12) {
    return std::nullopt;
  }

  UnderlineMetrics metrics;
  metrics.position = static_cast<double>(ReadInt16Be(*table, 8));
  metrics.thickness = static_cast<double>(ReadInt16Be(*table, 10));
  return metrics;
}

std::optional<UnderlineMetrics> TextBackendSimple::strikeoutMetrics(FontHandle font) const {
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return std::nullopt;
  }

  const auto table = fontManager_.sfntTable(font, "OS/2");
  if (!table || table->size() < 30) {
    return std::nullopt;
  }

  UnderlineMetrics metrics;
  metrics.thickness = static_cast<double>(ReadInt16Be(*table, 26));
  metrics.position = static_cast<double>(ReadInt16Be(*table, 28));
  return metrics;
}

std::optional<SubSuperMetrics> TextBackendSimple::subSuperMetrics(FontHandle font) const {
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return std::nullopt;
  }

  const auto table = fontManager_.sfntTable(font, "OS/2");
  if (!table || table->size() < 26) {
    return std::nullopt;
  }

  SubSuperMetrics metrics;
  // OS/2 table: ySubscriptYOffset at offset 16, ySuperscriptYOffset at offset 24 (int16 BE).
  metrics.subscriptYOffset = ReadInt16Be(*table, 16);
  metrics.superscriptYOffset = ReadInt16Be(*table, 24);
  return metrics;
}

Path TextBackendSimple::glyphOutline(FontHandle font, int glyphIndex, float scale) const {
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return {};
  }

  stbtt_vertex* vertices = nullptr;
  const int numVertices = stbtt_GetGlyphShape(info, glyphIndex, &vertices);

  if (vertices == nullptr) {
    return {};
  }
  if (numVertices <= 0) {
    // CFF glyphs may return an allocated zero-length shape. stb_truetype still
    // transfers ownership to the caller in that case.
    stbtt_FreeShape(info, vertices);
    return {};
  }

  PathBuilder builder;
  bool hasContour = false;

  for (int i = 0; i < numVertices; ++i) {
    const double x = static_cast<double>(vertices[i].x) * scale;
    // stb_truetype Y is up, SVG Y is down - flip.
    const double y = -static_cast<double>(vertices[i].y) * scale;

    switch (vertices[i].type) {
      case STBTT_vmove:
        if (hasContour) {
          builder.closePath();
        }
        builder.moveTo(Vector2d(x, y));
        hasContour = true;
        break;

      case STBTT_vline: builder.lineTo(Vector2d(x, y)); break;

      case STBTT_vcurve: {
        const double cx = static_cast<double>(vertices[i].cx) * scale;
        const double cy = -static_cast<double>(vertices[i].cy) * scale;
        builder.quadTo(Vector2d(cx, cy), Vector2d(x, y));
        break;
      }

      case STBTT_vcubic: {
        const double cx1 = static_cast<double>(vertices[i].cx) * scale;
        const double cy1 = -static_cast<double>(vertices[i].cy) * scale;
        const double cx2 = static_cast<double>(vertices[i].cx1) * scale;
        const double cy2 = -static_cast<double>(vertices[i].cy1) * scale;
        builder.curveTo(Vector2d(cx1, cy1), Vector2d(cx2, cy2), Vector2d(x, y));
        break;
      }

      default: break;
    }
  }

  if (hasContour) {
    builder.closePath();
  }

  stbtt_FreeShape(info, vertices);
  return builder.build();
}

bool TextBackendSimple::isBitmapOnly(FontHandle font) const {
  return fontManager_.isValidatedFont(font) && !HasCachedOutlineTables(fontManager_, font);
}

bool TextBackendSimple::isCursive(uint32_t /*codepoint*/) const {
  return false;
}

bool TextBackendSimple::hasSmallCapsFeature(FontHandle /*font*/) const {
  return false;
}

std::optional<TextBackend::BitmapGlyph> TextBackendSimple::bitmapGlyph(FontHandle /*font*/,
                                                                       int /*glyphIndex*/,
                                                                       float /*scale*/) const {
  return std::nullopt;
}

TextBackend::ShapedRun TextBackendSimple::shapeRun(FontHandle font, float fontSizePx,
                                                   std::string_view spanText, size_t byteOffset,
                                                   size_t byteLength, bool isVertical,
                                                   FontVariant fontVariant,
                                                   bool /*forceLogicalOrder*/) const {
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return {};
  }

  const float scale = scaleForPixelHeight(font, fontSizePx);
  if (scale == 0.0f) {
    return {};
  }

  ShapedRun result;
  int prevGlyph = 0;
  size_t pos = byteOffset;
  const size_t end = byteOffset + byteLength;

  while (pos < end) {
    const size_t startPos = pos;
    uint32_t codepoint = decodeUtf8(spanText, pos);

    // Small-caps synthesis: uppercase lowercase ASCII, apply reduced scale.
    bool smallCap = false;
    if (fontVariant == FontVariant::SmallCaps && codepoint >= 'a' && codepoint <= 'z') {
      codepoint = codepoint - 'a' + 'A';
      smallCap = true;
    }

    const int glyphIndex = stbtt_FindGlyphIndex(info, static_cast<int>(codepoint));
    int advanceWidth = 0;
    int lsb = 0;
    stbtt_GetGlyphHMetrics(info, glyphIndex, &advanceWidth, &lsb);

    const float glyphScale = smallCap ? scale * kSmallCapScale : scale;

    // Compute kerning from previous glyph to this one.
    double kernX = 0;
    double kernY = 0;
    if (prevGlyph != 0 && glyphIndex != 0) {
      const int kern = stbtt_GetGlyphKernAdvance(info, prevGlyph, glyphIndex);
      if (kern != 0) {
        if (isVertical && codepoint < 0x2E80) {
          kernY = static_cast<double>(kern) * scale;
        } else if (!isVertical) {
          kernX = static_cast<double>(kern) * scale;
        }
      }
    }

    ShapedGlyph glyph;
    glyph.glyphIndex = glyphIndex;
    glyph.cluster = static_cast<uint32_t>(startPos);
    glyph.fontSizeScale = smallCap ? kSmallCapScale : 1.0f;
    glyph.xKern = kernX;
    glyph.yKern = kernY;

    if (isVertical && codepoint < 0x2E80) {
      // Sideways Latin in vertical mode: horizontal advance becomes vertical.
      glyph.xAdvance = 0;
      glyph.yAdvance = static_cast<double>(advanceWidth) * glyphScale;
    } else if (isVertical) {
      // Upright CJK in vertical mode: advance = em height.
      glyph.xAdvance = 0;
      glyph.yAdvance = static_cast<double>(fontSizePx);
    } else {
      glyph.xAdvance = static_cast<double>(advanceWidth) * glyphScale;
      glyph.yAdvance = 0;
    }

    result.glyphs.push_back(glyph);
    prevGlyph = glyphIndex;
  }

  return result;
}

double TextBackendSimple::crossSpanKern(FontHandle prevFont, float prevSizePx,
                                        FontHandle /*curFont*/, float /*curSizePx*/,
                                        uint32_t prevCodepoint, uint32_t curCodepoint,
                                        bool isVertical) const {
  const stbtt_fontinfo* info = getFontInfo(prevFont);
  if (!info) {
    return 0.0;
  }

  const float scale = scaleForPixelHeight(prevFont, prevSizePx);
  const int prevGlyph = stbtt_FindGlyphIndex(info, static_cast<int>(prevCodepoint));
  const int curGlyph = stbtt_FindGlyphIndex(info, static_cast<int>(curCodepoint));
  const int kern = stbtt_GetGlyphKernAdvance(info, prevGlyph, curGlyph);
  if (kern == 0) {
    return 0.0;
  }

  if (isVertical && curCodepoint < 0x2E80) {
    return static_cast<double>(kern) * scale;
  }
  if (!isVertical) {
    return static_cast<double>(kern) * scale;
  }
  return 0.0;
}

}  // namespace donner::svg
