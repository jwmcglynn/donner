#include "donner/svg/renderer/TextBackendSimple.h"

#include "donner/base/Utf8.h"

#define STBTT_DEF extern
#include <stb/stb_truetype.h>

namespace donner::svg {

namespace {

/// Find an OpenType/TrueType table by 4-char tag in raw font data.
/// Returns the byte offset of the table, or 0 if not found.
int findFontTable(const unsigned char* data, int fontstart, const char* tag) {
  const int numTables = (data[fontstart + 4] << 8) | data[fontstart + 5];
  const int tabledir = fontstart + 12;
  for (int i = 0; i < numTables; ++i) {
    const int loc = tabledir + 16 * i;
    if (data[loc] == tag[0] && data[loc + 1] == tag[1] && data[loc + 2] == tag[2] &&
        data[loc + 3] == tag[3]) {
      return static_cast<int>((static_cast<unsigned>(data[loc + 8]) << 24) |
                              (static_cast<unsigned>(data[loc + 9]) << 16) |
                              (static_cast<unsigned>(data[loc + 10]) << 8) |
                              static_cast<unsigned>(data[loc + 11]));
    }
  }
  return 0;
}

/// Read a big-endian int16 from raw font data.
int16_t readInt16BE(const unsigned char* data, int offset) {
  return static_cast<int16_t>(static_cast<uint16_t>(data[offset] << 8) | data[offset + 1]);
}

/// Decode one UTF-8 codepoint from \p str starting at \p i. Advances \p i past the codepoint.
uint32_t decodeUtf8(std::string_view str, size_t& i) {
  const auto [cp, length] = Utf8::NextCodepoint(str.substr(i));
  i += static_cast<size_t>(length);
  return static_cast<uint32_t>(cp);
}

constexpr float kSmallCapScale = 0.8f;

}  // namespace

TextBackendSimple::TextBackendSimple(FontManager& fontManager) : fontManager_(fontManager) {}

FontVMetrics TextBackendSimple::fontVMetrics(FontHandle font) const {
  const stbtt_fontinfo* info = fontManager_.fontInfo(font);
  if (!info) {
    return {};
  }
  FontVMetrics metrics;
  stbtt_GetFontVMetrics(info, &metrics.ascent, &metrics.descent, &metrics.lineGap);
  return metrics;
}

float TextBackendSimple::scaleForPixelHeight(FontHandle font, float pixelHeight) const {
  return fontManager_.scaleForPixelHeight(font, pixelHeight);
}

float TextBackendSimple::scaleForEmToPixels(FontHandle font, float pixelHeight) const {
  const stbtt_fontinfo* info = fontManager_.fontInfo(font);
  if (!info) {
    return 0.0f;
  }
  return stbtt_ScaleForMappingEmToPixels(info, pixelHeight);
}

std::optional<UnderlineMetrics> TextBackendSimple::underlineMetrics(FontHandle font) const {
  const stbtt_fontinfo* info = fontManager_.fontInfo(font);
  if (!info) {
    return std::nullopt;
  }

  const int tab = findFontTable(info->data, info->fontstart, "post");
  if (!tab) {
    return std::nullopt;
  }

  UnderlineMetrics metrics;
  metrics.position = static_cast<double>(readInt16BE(info->data, tab + 8));
  metrics.thickness = static_cast<double>(readInt16BE(info->data, tab + 10));
  return metrics;
}

std::optional<SubSuperMetrics> TextBackendSimple::subSuperMetrics(FontHandle font) const {
  const stbtt_fontinfo* info = fontManager_.fontInfo(font);
  if (!info) {
    return std::nullopt;
  }

  const int tab = findFontTable(info->data, info->fontstart, "OS/2");
  if (!tab) {
    return std::nullopt;
  }

  SubSuperMetrics metrics;
  // OS/2 table: ySubscriptYOffset at offset 16, ySuperscriptYOffset at offset 24 (int16 BE).
  metrics.subscriptYOffset = readInt16BE(info->data, tab + 16);
  metrics.superscriptYOffset = readInt16BE(info->data, tab + 24);
  return metrics;
}

PathSpline TextBackendSimple::glyphOutline(FontHandle font, int glyphIndex, float scale) const {
  const stbtt_fontinfo* info = fontManager_.fontInfo(font);
  if (!info) {
    return {};
  }

  stbtt_vertex* vertices = nullptr;
  const int numVertices = stbtt_GetGlyphShape(info, glyphIndex, &vertices);

  PathSpline spline;
  if (numVertices <= 0 || vertices == nullptr) {
    return spline;
  }

  double curX = 0;
  double curY = 0;
  bool hasContour = false;

  for (int i = 0; i < numVertices; ++i) {
    const double x = static_cast<double>(vertices[i].x) * scale;
    // stb_truetype Y is up, SVG Y is down — flip.
    const double y = -static_cast<double>(vertices[i].y) * scale;

    switch (vertices[i].type) {
      case STBTT_vmove:
        if (hasContour) {
          spline.closePath();
        }
        spline.moveTo(Vector2d(x, y));
        hasContour = true;
        curX = x;
        curY = y;
        break;

      case STBTT_vline:
        spline.lineTo(Vector2d(x, y));
        curX = x;
        curY = y;
        break;

      case STBTT_vcurve: {
        // Quadratic bezier → convert to cubic.
        const double cx = static_cast<double>(vertices[i].cx) * scale;
        const double cy = -static_cast<double>(vertices[i].cy) * scale;
        const double cp1x = curX + (2.0 / 3.0) * (cx - curX);
        const double cp1y = curY + (2.0 / 3.0) * (cy - curY);
        const double cp2x = x + (2.0 / 3.0) * (cx - x);
        const double cp2y = y + (2.0 / 3.0) * (cy - y);
        spline.curveTo(Vector2d(cp1x, cp1y), Vector2d(cp2x, cp2y), Vector2d(x, y));
        curX = x;
        curY = y;
        break;
      }

      case STBTT_vcubic: {
        const double cx1 = static_cast<double>(vertices[i].cx) * scale;
        const double cy1 = -static_cast<double>(vertices[i].cy) * scale;
        const double cx2 = static_cast<double>(vertices[i].cx1) * scale;
        const double cy2 = -static_cast<double>(vertices[i].cy1) * scale;
        spline.curveTo(Vector2d(cx1, cy1), Vector2d(cx2, cy2), Vector2d(x, y));
        curX = x;
        curY = y;
        break;
      }

      default: break;
    }
  }

  if (hasContour) {
    spline.closePath();
  }

  stbtt_FreeShape(info, vertices);
  return spline;
}

bool TextBackendSimple::isBitmapOnly(FontHandle font) const {
  return fontManager_.isBitmapOnly(font);
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
  const stbtt_fontinfo* info = fontManager_.fontInfo(font);
  if (!info) {
    return {};
  }

  const float scale = fontManager_.scaleForPixelHeight(font, fontSizePx);
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
  const stbtt_fontinfo* info = fontManager_.fontInfo(prevFont);
  if (!info) {
    return 0.0;
  }

  const float scale = fontManager_.scaleForPixelHeight(prevFont, prevSizePx);
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
