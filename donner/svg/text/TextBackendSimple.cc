#include "donner/svg/text/TextBackendSimple.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "donner/base/Utf8.h"
#include "donner/svg/text/BidiParagraph.h"
#define STBTT_DEF extern
#include <stb/stb_truetype.h>

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

/// Decode one UTF-8 codepoint from \p str starting at \p i. Advances \p i past the codepoint.
uint32_t decodeUtf8(std::string_view str, size_t& i) {
  const auto [cp, length] = Utf8::NextCodepoint(str.substr(i));
  i += static_cast<size_t>(length);
  return static_cast<uint32_t>(cp);
}

constexpr float kSmallCapScale = 0.8f;

Path SyntheticOblique(const Path& upright) {
  const Transform2d obliqueFromUpright = Transform2d::SkewX(-std::atan(0.22));
  const auto point = [&](const Vector2d& uprightPoint) {
    return obliqueFromUpright.transformPosition(uprightPoint);
  };
  PathBuilder builder;
  upright.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo: builder.moveTo(point(points[0])); break;
      case Path::Verb::LineTo: builder.lineTo(point(points[0])); break;
      case Path::Verb::QuadTo: builder.quadTo(point(points[0]), point(points[1])); break;
      case Path::Verb::CurveTo:
        builder.curveTo(point(points[0]), point(points[1]), point(points[2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  });
  return builder.build();
}

bool HasCachedOutlineTables(const FontManager& fontManager, FontHandle font) {
  return fontManager.sfntTable(font, "glyf").has_value() ||
         fontManager.sfntTable(font, "CFF ").has_value() ||
         fontManager.sfntTable(font, "CFF2").has_value();
}

bool HasIntrinsicItalicStyle(const FontManager& fontManager, FontHandle font) {
  if (const auto os2 = fontManager.sfntTable(font, "OS/2"); os2 && os2->size() >= 64) {
    const uint16_t selection = static_cast<uint16_t>(ReadInt16Be(*os2, 62));
    return (selection & ((1u << 0) | (1u << 9))) != 0;
  }
  if (const auto head = fontManager.sfntTable(font, "head"); head && head->size() >= 46) {
    return (static_cast<uint16_t>(ReadInt16Be(*head, 44)) & (1u << 1)) != 0;
  }
  return false;
}

/// Cached stb_truetype parse state attached to a font entity.
struct StbFontComponent {
  stbtt_fontinfo fontInfo{};
  bool valid = false;
  bool intrinsicItalic = false;
};

Path OutlineFromStbVertices(const stbtt_vertex* vertices, int numVertices, float scale) {
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
  return builder.build();
}

bool NeedsSyntheticOblique(const FontManager& manager, const StbFontComponent& cached,
                           FontHandle font) {
  const auto request = manager.providerFaceRequest(font);
  return request && request->style != FontStyle::Normal && !cached.intrinsicItalic &&
         (manager.isImmutableCatalogFont(font) || manager.isGenericSansProviderFont(font));
}

}  // namespace

TextBackendSimple::TextBackendSimple(FontManager& fontManager, Registry& registry)
    : fontManager_(fontManager), registry_(registry) {}

const stbtt_fontinfo* TextBackendSimple::getFontInfo(FontHandle font) const {
  if (!font) {
    return nullptr;
  }

  // stb_truetype has no length-aware initialization API and performs unchecked reads within
  // individual SFNT tables. Never expose document-provided bytes to it, even after outer table
  // bounds validation. The full backend uses FreeType's length-aware memory-face API instead.
  if (!fontManager_.isTrustedFont(font)) {
    return nullptr;
  }

  if (const auto* cached = registry_.try_get<StbFontComponent>(font.entity())) {
    return cached->valid ? &cached->fontInfo : nullptr;
  }

  if (!fontManager_.isValidatedFont(font)) {
    return nullptr;
  }
  auto& cached = registry_.emplace<StbFontComponent>(font.entity());

  if (!HasCachedOutlineTables(fontManager_, font)) {
    return nullptr;
  }

  const auto fontData = fontManager_.fontData(font);
  if (stbtt_InitFont(&cached.fontInfo, fontData.data(), 0)) {
    cached.valid = true;
    cached.intrinsicItalic = HasIntrinsicItalicStyle(fontManager_, font);
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
  metrics.unitsPerEm = ReadUnitsPerEm(fontManager_, font);

  // x-height from the OS/2 table (`sxHeight`, offset 86), present in version >= 2.
  if (const auto os2 = fontManager_.sfntTable(font, "OS/2"); os2 && os2->size() >= 88) {
    const uint16_t version = static_cast<uint16_t>(ReadInt16Be(*os2, 0));
    if (version >= 2) {
      metrics.xHeight = ReadInt16Be(*os2, 86);
    }
  }
  if (metrics.xHeight <= 0) {
    const int glyph = stbtt_FindGlyphIndex(info, 'x');
    int top = 0;
    if (glyph != 0 && stbtt_GetGlyphBox(info, glyph, nullptr, nullptr, nullptr, &top)) {
      metrics.xHeight = std::max(top, 0);
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
  return glyphOutline(font, glyphIndex, scale, true);
}

Path TextBackendSimple::glyphOutline(FontHandle font, int glyphIndex, float scale,
                                     bool allowSyntheticOblique) const {
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

  Path outline = OutlineFromStbVertices(vertices, numVertices, scale);
  stbtt_FreeShape(info, vertices);
  const auto* cached = registry_.try_get<StbFontComponent>(font.entity());
  if (allowSyntheticOblique && cached != nullptr &&
      NeedsSyntheticOblique(fontManager_, *cached, font)) {
    outline = SyntheticOblique(outline);
  }
  return outline;
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
                                                   bool forceLogicalOrder) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      TextOrientation::Mixed, fontVariant, true, forceLogicalOrder);
}

TextBackend::ShapedRun TextBackendSimple::shapeRun(FontHandle font, float fontSizePx,
                                                   std::string_view spanText, size_t byteOffset,
                                                   size_t byteLength, bool isVertical,
                                                   TextOrientation textOrientation,
                                                   FontVariant fontVariant,
                                                   bool forceLogicalOrder) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      textOrientation, fontVariant, true, forceLogicalOrder);
}

TextBackend::ShapedRun TextBackendSimple::shapeRunNoKerning(
    FontHandle font, float fontSizePx, std::string_view spanText, size_t byteOffset,
    size_t byteLength, bool isVertical, FontVariant fontVariant, bool forceLogicalOrder) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      TextOrientation::Mixed, fontVariant, false, forceLogicalOrder);
}

TextBackend::ShapedRun TextBackendSimple::shapeRunNoKerning(
    FontHandle font, float fontSizePx, std::string_view spanText, size_t byteOffset,
    size_t byteLength, bool isVertical, TextOrientation textOrientation, FontVariant fontVariant,
    bool forceLogicalOrder) const {
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      textOrientation, fontVariant, false, forceLogicalOrder);
}

TextBackend::ShapedRun TextBackendSimple::shapeRunInContext(
    FontHandle font, float fontSizePx, std::string_view spanText, const ShapingContext& context,
    size_t byteOffset, size_t byteLength, bool isVertical, TextOrientation textOrientation,
    FontVariant fontVariant, bool kerning) const {
  if (context.work && !context.work->consume(byteLength)) {
    return {};
  }
  bool backward = false;
#ifdef DONNER_TEXT_ENABLED
  backward = !isVertical && context.rightToLeft.value_or(false);
#endif
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      textOrientation, fontVariant, kerning, false, backward);
}

namespace {

bool UsesSidewaysMetrics(TextOrientation orientation, uint32_t codepoint) {
  return orientation == TextOrientation::Sideways ||
         (orientation == TextOrientation::Mixed && codepoint < 0x2E80);
}

/// Returns the pair adjustment in horizontal or sideways-vertical text.
Vector2d SimplePairKerning(const stbtt_fontinfo* info, int prevGlyph, int glyphIndex, float scale,
                           bool isVertical, TextOrientation textOrientation, uint32_t codepoint,
                           bool enableKerning) {
  // Compute kerning from previous glyph to this one.
  double kernX = 0;
  double kernY = 0;
  if (enableKerning && prevGlyph != 0 && glyphIndex != 0) {
    const int kern = stbtt_GetGlyphKernAdvance(info, prevGlyph, glyphIndex);
    if (kern != 0) {
      const bool sideways = UsesSidewaysMetrics(textOrientation, codepoint);
      if (isVertical && sideways) {
        kernY = static_cast<double>(kern) * scale;
      } else if (!isVertical) {
        kernX = static_cast<double>(kern) * scale;
      }
    }
  }

  return Vector2d(kernX, kernY);
}

/// Sets the advance axis for horizontal, sideways Latin, or upright CJK text.
void SetSimpleGlyphAdvance(TextBackend::ShapedGlyph& glyph, bool isVertical,
                           TextOrientation textOrientation, uint32_t codepoint,
                           const stbtt_fontinfo* info, int glyphIndex, int advanceWidth,
                           float glyphScale, float fontSizePx) {
  const bool sideways = UsesSidewaysMetrics(textOrientation, codepoint);
  if (isVertical && sideways) {
    // Sideways Latin in vertical mode: horizontal advance becomes vertical.
    glyph.xAdvance = 0;
    glyph.yAdvance = static_cast<double>(advanceWidth) * glyphScale;
  } else if (isVertical && textOrientation == TextOrientation::Upright) {
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
    const double cellAdvance = static_cast<double>(ascent - descent) * glyphScale;
    glyph.xAdvance = 0;
    glyph.yAdvance = advanceWidth == 0 ? 0.0 : cellAdvance;
    int xMin = 0;
    int yMin = 0;
    int xMax = 0;
    int yMax = 0;
    if (stbtt_GetGlyphBox(info, glyphIndex, &xMin, &yMin, &xMax, &yMax)) {
      glyph.xOffset = -static_cast<double>(advanceWidth) * glyphScale / 2.0;
      glyph.yOffset = static_cast<double>(ascent) * glyphScale;
      if (advanceWidth == 0) {
        glyph.yOffset -= cellAdvance;
      }
    }
  } else if (isVertical) {
    glyph.xAdvance = 0;
    glyph.yAdvance = advanceWidth == 0 ? 0.0 : static_cast<double>(fontSizePx);
  } else {
    glyph.xAdvance = static_cast<double>(advanceWidth) * glyphScale;
    glyph.yAdvance = 0;
  }
}

}  // namespace

namespace {

struct SimpleScalar {
  size_t byte;
  uint32_t codepoint;
};

/// Iterates a borrowed shaping range while its synchronous caller retains the source text.
class SimpleScalarSequence {
public:
  SimpleScalarSequence(std::string_view text, size_t offset, size_t length, bool backward)
      : text_(text.substr(0, offset + length)), position_(offset) {
#ifdef DONNER_TEXT_ENABLED
    backward_ = backward;
    if (backward_) {
      prepareBackward();
    }
#else
    (void)backward;
#endif
  }

  std::optional<SimpleScalar> next() {
#ifdef DONNER_TEXT_ENABLED
    if (backward_) {
      if (next_ == ordered_.size()) {
        return std::nullopt;
      }
      auto scalar = ordered_[next_++];
      scalar.codepoint = BidiParagraph::Mirror(scalar.codepoint);
      return scalar;
    }
#endif
    return nextForward();
  }

private:
  std::optional<SimpleScalar> nextForward() {
    while (position_ < text_.size()) {
      const size_t byte = position_;
      const uint32_t cp = decodeUtf8(text_, position_);
      if (!BidiParagraph::IsControl(cp)) {
        return SimpleScalar{byte, cp};
      }
    }
    return std::nullopt;
  }

#ifdef DONNER_TEXT_ENABLED
  void prepareBackward() {
    std::vector<SimpleScalar> scalars;
    std::vector<std::pair<size_t, size_t>> groups;
    bool previousJoiner = false;
    while (const auto scalar = nextForward()) {
      if (groups.empty() || (!BidiParagraph::IsNonspacing(scalar->codepoint) && !previousJoiner)) {
        groups.emplace_back(scalars.size(), scalars.size());
      }
      scalars.push_back(*scalar);
      groups.back().second = scalars.size();
      previousJoiner = scalar->codepoint == 0x200d;
    }
    ordered_.reserve(scalars.size());
    for (auto group = groups.rbegin(); group != groups.rend(); ++group) {
      ordered_.insert(ordered_.end(), scalars.begin() + group->first,
                      scalars.begin() + group->second);
    }
  }

#endif  // DONNER_TEXT_ENABLED

  std::string_view text_;
  size_t position_;
#ifdef DONNER_TEXT_ENABLED
  bool backward_ = false;
  size_t next_ = 0;
  std::vector<SimpleScalar> ordered_;
#endif
};

struct SimpleShapingMetrics {
  const stbtt_fontinfo* info;
  float fontSizePx;
  float scale;
  bool vertical;
  TextOrientation orientation;
  FontVariant variant;
  bool kerning;

  TextBackend::ShapedGlyph glyph(SimpleScalar scalar, int previousGlyph) const {
    const bool smallCap =
        variant == FontVariant::SmallCaps && scalar.codepoint >= 'a' && scalar.codepoint <= 'z';
    if (smallCap) {
      scalar.codepoint = scalar.codepoint - 'a' + 'A';
    }
    const int index = stbtt_FindGlyphIndex(info, static_cast<int>(scalar.codepoint));
    int advanceWidth = 0;
    int lsb = 0;
    stbtt_GetGlyphHMetrics(info, index, &advanceWidth, &lsb);
    const float glyphScale = smallCap ? scale * kSmallCapScale : scale;
    const Vector2d pair = SimplePairKerning(info, previousGlyph, index, scale, vertical,
                                            orientation, scalar.codepoint, kerning);
    TextBackend::ShapedGlyph result;
    result.glyphIndex = index;
    result.cluster = static_cast<uint32_t>(scalar.byte);
    result.fontSizeScale = smallCap ? kSmallCapScale : 1.0f;
    result.xKern = pair.x;
    result.yKern = pair.y;
    SetSimpleGlyphAdvance(result, vertical, orientation, scalar.codepoint, info, index,
                          advanceWidth, glyphScale, fontSizePx);
    return result;
  }
};

bool ValidSimpleShapingRange(float size, std::string_view text, size_t offset, size_t length) {
  return std::isfinite(size) && size > 0 && offset <= text.size() && length <= text.size() - offset;
}

}  // namespace

TextBackend::ShapedRun TextBackendSimple::shapeRunImpl(
    FontHandle font, float fontSizePx, std::string_view spanText, size_t byteOffset,
    size_t byteLength, bool isVertical, TextOrientation textOrientation, FontVariant fontVariant,
    bool enableKerning, bool /*forceLogicalOrder*/, bool backward) const {
  if (!ValidSimpleShapingRange(fontSizePx, spanText, byteOffset, byteLength)) {
    return {};
  }
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return {};
  }
  const float scale = scaleForPixelHeight(font, fontSizePx);
  if (scale == 0) {
    return {};
  }
  const SimpleShapingMetrics metrics{info,        fontSizePx,   scale, isVertical, textOrientation,
                                     fontVariant, enableKerning};
  SimpleScalarSequence sequence(spanText, byteOffset, byteLength, backward);
  ShapedRun result;
  result.backward = backward;
  int previousGlyph = 0;
  while (const auto scalar = sequence.next()) {
    result.glyphs.push_back(metrics.glyph(*scalar, previousGlyph));
    previousGlyph = result.glyphs.back().glyphIndex;
  }
  return result;
}

double TextBackendSimple::crossSpanKern(FontHandle previousFont, float previousSize,
                                        FontHandle currentFont, float currentSize,
                                        uint32_t previousCodepoint, uint32_t currentCodepoint,
                                        bool vertical, TextOrientation orientation,
                                        const ShapingContext& context) const {
#ifdef DONNER_TEXT_ENABLED
  if (context.rightToLeft.value_or(false)) {
    previousCodepoint = BidiParagraph::Mirror(previousCodepoint);
    currentCodepoint = BidiParagraph::Mirror(currentCodepoint);
  }
#else
  (void)context;
#endif
  return crossSpanKern(previousFont, previousSize, currentFont, currentSize, previousCodepoint,
                       currentCodepoint, vertical, orientation);
}

double TextBackendSimple::crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle curFont,
                                        float curSizePx, uint32_t prevCodepoint,
                                        uint32_t curCodepoint, bool isVertical) const {
  return crossSpanKern(prevFont, prevSizePx, curFont, curSizePx, prevCodepoint, curCodepoint,
                       isVertical, TextOrientation::Mixed);
}

double TextBackendSimple::crossSpanKern(FontHandle prevFont, float prevSizePx, FontHandle curFont,
                                        float curSizePx, uint32_t prevCodepoint,
                                        uint32_t curCodepoint, bool isVertical,
                                        TextOrientation textOrientation) const {
  if (!fontManager_.fontsShareFamily(prevFont, curFont) || !std::isfinite(prevSizePx) ||
      !std::isfinite(curSizePx) || prevSizePx <= 0.0f || curSizePx <= 0.0f) {
    return 0.0;
  }
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

  const bool sideways = UsesSidewaysMetrics(textOrientation, curCodepoint);
  if (isVertical && sideways) {
    return static_cast<double>(kern) * scale;
  }
  if (!isVertical) {
    return static_cast<double>(kern) * scale;
  }
  return 0.0;
}

}  // namespace donner::svg
