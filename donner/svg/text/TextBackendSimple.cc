#include "donner/svg/text/TextBackendSimple.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "donner/base/Utf8.h"
#include "donner/svg/text/BidiParagraph.h"
#define STBTT_DEF extern
#include <stb/stb_truetype.h>

namespace donner::svg {

namespace {
/// Bounds-checked big-endian reads within one SFNT table.
class SfntTableReader {
public:
  explicit SfntTableReader(std::span<const uint8_t> table) : table_(table) {}

  /// Reads the 16-bit value at \p offset, or nullopt past the end of the table.
  std::optional<uint16_t> u16(size_t offset) const {
    if (offset > table_.size() || table_.size() - offset < 2) {
      return std::nullopt;
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(table_[offset]) << 8) | table_[offset + 1]);
  }

  /// Reads the 32-bit value at \p offset, or nullopt past the end of the table.
  std::optional<uint32_t> u32(size_t offset) const {
    const auto high = u16(offset);
    const auto low = u16(offset + 2);
    if (!high || !low) {
      return std::nullopt;
    }
    return (static_cast<uint32_t>(*high) << 16) | *low;
  }

  /// Returns the table's size in bytes.
  size_t size() const { return table_.size(); }

  /// Returns true if the four bytes at \p offset spell \p tag.
  bool hasTag(size_t offset, std::string_view tag) const {
    return offset <= table_.size() && table_.size() - offset >= 4 &&
           std::equal(tag.begin(), tag.end(), table_.begin() + static_cast<ptrdiff_t>(offset));
  }

private:
  std::span<const uint8_t> table_;
};

/// Reads the signed 16-bit value at \p offset of \p data, or 0 past its end.
int16_t ReadInt16Be(std::span<const uint8_t> data, size_t offset) {
  return static_cast<int16_t>(SfntTableReader(data).u16(offset).value_or(0));
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

/// OpenType encodes four-character tags in big-endian order.
constexpr uint32_t LayoutTag(const char (&tag)[5]) {
  return (static_cast<uint32_t>(tag[0]) << 24) | (static_cast<uint32_t>(tag[1]) << 16) |
         (static_cast<uint32_t>(tag[2]) << 8) | static_cast<uint32_t>(tag[3]);
}

/// Preferred GSUB script tags, followed by the default-script fallbacks.
std::array<uint32_t, 6> OpenTypeScriptTags(uint32_t script) {
  uint32_t legacy = script == 0 ? 0 : script | 0x20000000;
  switch (script) {
    case LayoutTag("Hira"): legacy = LayoutTag("kana"); break;
    case LayoutTag("Laoo"): legacy = LayoutTag("lao "); break;
    case LayoutTag("Yiii"): legacy = LayoutTag("yi  "); break;
    case LayoutTag("Nkoo"): legacy = LayoutTag("nko "); break;
    case LayoutTag("Vaii"): legacy = LayoutTag("vai "); break;
    default: break;
  }

  constexpr std::pair<uint32_t, uint32_t> versioned[] = {
      {LayoutTag("Beng"), LayoutTag("bng2")}, {LayoutTag("Deva"), LayoutTag("dev2")},
      {LayoutTag("Gujr"), LayoutTag("gjr2")}, {LayoutTag("Guru"), LayoutTag("gur2")},
      {LayoutTag("Knda"), LayoutTag("knd2")}, {LayoutTag("Mlym"), LayoutTag("mlm2")},
      {LayoutTag("Orya"), LayoutTag("ory2")}, {LayoutTag("Taml"), LayoutTag("tml2")},
      {LayoutTag("Telu"), LayoutTag("tel2")}, {LayoutTag("Mymr"), LayoutTag("mym2")},
  };
  uint32_t version2 = 0;
  for (const auto& [iso, tag] : versioned) {
    if (script == iso) {
      version2 = tag;
      break;
    }
  }
  // Myanmar has no version 3 script tag.
  const uint32_t version3 = version2 != 0 && script != LayoutTag("Mymr") ? version2 + 1 : 0;
  return {version3, version2, legacy, LayoutTag("DFLT"), LayoutTag("dflt"), LayoutTag("latn")};
}

/**
 * The face's native small-cap glyphs: the single substitutions of its GSUB `smcp` feature, which
 * the full backend applies through HarfBuzz.
 *
 * The selected script and language system supply the lookups in lookup-list order. Only single
 * substitution subtables are read (lookup type 1, also inside an extension, type 7). Lookup flags
 * are not applied. A bounded number of language systems is cached per trusted face.
 */
class SimpleSmallCapsSubstitution {
public:
  /// Steps (script, feature, lookup and subtable records visited) one read may take.
  static constexpr size_t kMaximumReadSteps = size_t{1} << 16;
  /// Single substitution subtables one face may contribute; each glyph is checked against all.
  static constexpr size_t kMaximumSubtables = 64;

  /// Resolves a GSUB language system for the requested Unicode script and OpenType language.
  static size_t LanguageSystem(std::span<const uint8_t> table, uint32_t scriptTag,
                               std::span<const uint32_t> languageTags) {
    const SfntTableReader reader(table);
    const auto scripts = reader.u16(4);
    if (!scripts || *scripts == 0) {
      return 0;
    }
    const auto count = reader.u16(*scripts).value_or(0);
    size_t scriptOffset = 0;
    for (const uint32_t tag : OpenTypeScriptTags(scriptTag)) {
      const auto offset = FindTaggedOffset(reader, *scripts + 2, count, tag);
      if (offset != 0) {
        scriptOffset = *scripts + offset;
        break;
      }
    }
    if (scriptOffset == 0) {
      return 0;
    }
    const auto languageCount = reader.u16(scriptOffset + 2).value_or(0);
    uint16_t offset = 0;
    for (const uint32_t tag : languageTags) {
      offset = FindTaggedOffset(reader, scriptOffset + 4, languageCount, tag);
      if (offset != 0) {
        break;
      }
    }
    if (offset == 0) {
      offset = FindTaggedOffset(reader, scriptOffset + 4, languageCount, LayoutTag("dflt"));
    }
    const size_t langSys = offset != 0 ? offset : reader.u16(scriptOffset).value_or(0);
    return langSys == 0 ? 0 : scriptOffset + langSys;
  }

  /// Reads the `smcp` lookups of the selected language system in \p gsub.
  static SimpleSmallCapsSubstitution Read(std::span<const uint8_t> gsub, size_t langSys,
                                          size_t& steps) {
    SimpleSmallCapsSubstitution result;
    const SfntTableReader reader(gsub);
    const auto lookupList = reader.u16(8);
    if (!lookupList) {
      return result;
    }
    size_t subtableCount = 0;
    const uint16_t lookupCount = reader.u16(*lookupList).value_or(0);
    for (const uint16_t index : SmcpLookupIndices(reader, langSys, steps)) {
      const auto lookupOffset = reader.u16(*lookupList + 2 + size_t{index} * 2);
      if (index >= lookupCount || !lookupOffset) {
        continue;
      }
      std::vector<size_t> subtables =
          SingleSubstitutionSubtables(reader, size_t{*lookupList} + *lookupOffset, steps);
      subtableCount += subtables.size();
      if (!subtables.empty()) {
        result.lookups_.push_back(std::move(subtables));
      }
    }
    if (steps == 0 || subtableCount > kMaximumSubtables) {
      return SimpleSmallCapsSubstitution();
    }
    result.gsub_ = gsub;
    return result;
  }

  /// Returns true if the face has no native small caps.
  bool empty() const { return lookups_.empty(); }

  /// Returns the small-cap glyph for \p glyph, which is \p glyph itself where no lookup covers it.
  int substitute(int glyph) const {
    const SfntTableReader reader(gsub_);
    for (const auto& subtables : lookups_) {
      for (const size_t subtable : subtables) {
        const auto coverage = reader.u16(subtable + 2);
        const auto coverageIndex =
            coverage ? CoverageIndex(reader, subtable + *coverage, glyph) : std::nullopt;
        if (!coverageIndex) {
          continue;
        }
        const auto format = reader.u16(subtable);
        if (format == 1) {
          // deltaGlyphID is added modulo 65536.
          glyph = static_cast<uint16_t>(glyph + reader.u16(subtable + 4).value_or(0));
        } else if (format == 2 && *coverageIndex < reader.u16(subtable + 4).value_or(0)) {
          glyph = reader.u16(subtable + 6 + *coverageIndex * 2).value_or(glyph);
        }
        // Within a lookup, only the first subtable that covers the glyph applies.
        break;
      }
    }
    return glyph;
  }

private:
  /// Spends one of \p steps, returning false once none are left.
  static bool Spend(size_t& steps) {
    if (steps == 0) {
      return false;
    }
    --steps;
    return true;
  }

  /// Looks up a tag in an OpenType list of sorted six-byte tag/offset records.
  static uint16_t FindTaggedOffset(const SfntTableReader& reader, size_t records, size_t count,
                                   uint32_t tag) {
    if (tag == 0) {
      return 0;
    }
    size_t first = 0;
    while (first < count) {
      const size_t middle = first + (count - first) / 2;
      const size_t record = records + middle * 6;
      const auto value = reader.u32(record);
      if (!value) {
        return 0;
      }
      if (*value == tag) {
        return reader.u16(record + 4).value_or(0);
      }
      if (*value < tag) {
        first = middle + 1;
      } else {
        count = middle;
      }
    }
    return 0;
  }

  /// Returns the sorted, distinct lookup indices for the selected language system.
  static std::vector<uint16_t> SmcpLookupIndices(const SfntTableReader& reader, size_t langSys,
                                                 size_t& steps) {
    std::vector<uint16_t> indices;
    const auto featureList = reader.u16(6);
    if (featureList && *featureList != 0) {
      AppendSmcpLookups(reader, *featureList, langSys, indices, steps);
    }
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    return indices;
  }

  /// Appends the lookup indices of the `smcp` features the language system at \p langSys lists.
  static void AppendSmcpLookups(const SfntTableReader& reader, size_t featureList, size_t langSys,
                                std::vector<uint16_t>& indices, size_t& steps) {
    const uint16_t featureRecords = reader.u16(featureList).value_or(0);
    const uint16_t featureCount = reader.u16(langSys + 4).value_or(0);
    for (size_t i = 0; i <= featureCount && Spend(steps); ++i) {
      const auto featureIndex = reader.u16(i == 0 ? langSys + 2 : langSys + 4 + i * 2);
      const size_t record = featureList + 2 + size_t{featureIndex.value_or(0)} * 6;
      const auto featureOffset = reader.u16(record + 4);
      if (!featureIndex || *featureIndex >= featureRecords || !featureOffset ||
          !reader.hasTag(record, "smcp")) {
        continue;
      }
      const size_t feature = featureList + *featureOffset;
      const uint16_t lookupCount = reader.u16(feature + 2).value_or(0);
      for (size_t lookup = 0; lookup < lookupCount && Spend(steps); ++lookup) {
        if (const auto index = reader.u16(feature + 4 + lookup * 2)) {
          indices.push_back(*index);
        }
      }
    }
  }

  /// Returns the single substitution subtables of the lookup at \p lookup, resolving extension
  /// subtables; any other lookup type has none.
  static std::vector<size_t> SingleSubstitutionSubtables(const SfntTableReader& reader,
                                                         size_t lookup, size_t& steps) {
    std::vector<size_t> subtables;
    const auto type = reader.u16(lookup);
    if (type != 1 && type != 7) {
      return subtables;
    }
    const uint16_t subtableCount = reader.u16(lookup + 4).value_or(0);
    for (size_t i = 0; i < subtableCount && Spend(steps); ++i) {
      const auto subtableOffset = reader.u16(lookup + 6 + i * 2);
      if (!subtableOffset) {
        continue;
      }
      const size_t subtable = lookup + *subtableOffset;
      if (type == 1) {
        subtables.push_back(subtable);
        // Requiring the 32-bit offset to be smaller than the table keeps the sum from wrapping
        // where size_t has 32 bits; the bounds-checked reads keep every access inside the table.
      } else if (const auto extension = reader.u32(subtable + 4);
                 extension && *extension < reader.size() && reader.u16(subtable + 2) == 1) {
        subtables.push_back(subtable + *extension);
      }
    }
    return subtables;
  }

  /// Returns the coverage index of \p glyph in the coverage table at \p coverage.
  static std::optional<size_t> CoverageIndex(const SfntTableReader& reader, size_t coverage,
                                             int glyph) {
    const auto format = reader.u16(coverage);
    const uint16_t count = reader.u16(coverage + 2).value_or(0);
    // Glyph arrays and ranges are sorted, so search them by bisection.
    size_t low = 0;
    size_t high = count;
    while (low < high) {
      const size_t middle = low + (high - low) / 2;
      int first = 0;
      int last = 0;
      size_t firstIndex = middle;
      if (format == 1) {
        const auto value = reader.u16(coverage + 4 + middle * 2);
        if (!value) {
          return std::nullopt;
        }
        first = last = *value;
      } else if (format == 2) {
        const size_t range = coverage + 4 + middle * 6;
        const auto start = reader.u16(range);
        const auto end = reader.u16(range + 2);
        const auto startIndex = reader.u16(range + 4);
        if (!start || !end || !startIndex) {
          return std::nullopt;
        }
        first = *start;
        last = *end;
        firstIndex = *startIndex;
      } else {
        return std::nullopt;
      }
      if (glyph < first) {
        high = middle;
      } else if (glyph > last) {
        low = middle + 1;
      } else {
        return firstIndex + static_cast<size_t>(glyph - first);
      }
    }
    return std::nullopt;
  }

  std::span<const uint8_t> gsub_;
  std::vector<std::vector<size_t>> lookups_;  //!< Single substitution subtables of each lookup.
};

/// Cached stb_truetype parse state attached to a font entity.
struct StbFontComponent {
  stbtt_fontinfo fontInfo{};
  bool valid = false;
  bool intrinsicItalic = false;
  /// Small-cap lookups keyed by the selected language-system offset within this face's GSUB.
  std::unordered_map<size_t, SimpleSmallCapsSubstitution> smallCaps;
  std::array<size_t, 32> smallCapsOrder{};
  size_t smallCapsNext = 0;
};

bool AdmitSimpleSmallCapsScript(const TextBackend::ShapingContext& context, size_t length) {
  return !context.work ||
         context.work->consume(length, context.simpleUnicodeScript || length >= context.byteLength
                                           ? 0
                                           : context.byteLength);
}

uint32_t DetectUnicodeScript(std::string_view text) {
  for (size_t offset = 0; offset < text.size();) {
    const uint32_t scalar = decodeUtf8(text, offset);
#ifdef DONNER_TEXT_ENABLED
    const uint32_t script = BidiParagraph::StrongScript(scalar);
    if (script != 0) {
      return script;
    }
#else
    if ((scalar >= 'A' && scalar <= 'Z') || (scalar >= 'a' && scalar <= 'z')) {
      return LayoutTag("Latn");
    }
#endif
  }
  return 0;
}

uint32_t ShapingScript(std::string_view text, size_t offset, size_t length,
                       const TextBackend::ShapingContext* context) {
  const uint32_t script = DetectUnicodeScript(text.substr(offset, length));
  if (!context || context->byteLength <= length || context->byteOffset > text.size() ||
      context->byteLength > text.size() - context->byteOffset) {
    return script;
  }
  if (!context->simpleUnicodeScript) {
    context->simpleUnicodeScript =
        DetectUnicodeScript(text.substr(context->byteOffset, context->byteLength));
  }
  return script != 0 ? script : *context->simpleUnicodeScript;
}

/// Reuses the selected language system's bounded lookup parse across runs of this face.
const SimpleSmallCapsSubstitution* CachedSmallCaps(Registry& registry, const FontManager& manager,
                                                   FontHandle font, uint32_t script,
                                                   std::span<const uint32_t> languages) {
  const auto gsub = manager.sfntTable(font, "GSUB");
  const size_t langSys =
      gsub ? SimpleSmallCapsSubstitution::LanguageSystem(*gsub, script, languages) : 0;
  if (langSys == 0) {
    return nullptr;
  }
  StbFontComponent& cached = registry.get<StbFontComponent>(font.entity());
  const auto found = cached.smallCaps.find(langSys);
  if (found != cached.smallCaps.end()) {
    return &found->second;
  }
  // Keep cache capacity independent of which scripts have already been requested. Eviction changes
  // the cost of reusing a language system, but the same bounded parse always selects its result.
  cached.smallCaps.erase(cached.smallCapsOrder[cached.smallCapsNext]);
  cached.smallCapsOrder[cached.smallCapsNext] = langSys;
  cached.smallCapsNext = (cached.smallCapsNext + 1) % cached.smallCapsOrder.size();
  size_t steps = SimpleSmallCapsSubstitution::kMaximumReadSteps;
  const auto it =
      cached.smallCaps.emplace(langSys, SimpleSmallCapsSubstitution::Read(*gsub, langSys, steps));
  return &it.first->second;
}

const SimpleSmallCapsSubstitution* SmallCapsForRun(Registry& registry, const FontManager& manager,
                                                   FontHandle font, FontVariant variant,
                                                   std::string_view text, size_t offset,
                                                   size_t length,
                                                   const TextBackend::ShapingContext* context) {
  return variant == FontVariant::SmallCaps
             ? CachedSmallCaps(registry, manager, font,
                               ShapingScript(text, offset, length, context),
                               context ? std::span<const uint32_t>(context->openTypeLanguageTags)
                                       : std::span<const uint32_t>())
             : nullptr;
}

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

bool TextBackendSimple::hasSmallCapsFeature(FontHandle font) const {
  return hasSmallCapsFeatureForRun(font, "a", 0, 1, false, TextOrientation::Mixed, nullptr);
}

bool TextBackendSimple::hasSmallCapsFeatureForRun(FontHandle font, std::string_view text,
                                                  size_t offset, size_t length, bool /*vertical*/,
                                                  TextOrientation /*orientation*/,
                                                  const ShapingContext* context) const {
  if (offset > text.size() || length > text.size() - offset || !getFontInfo(font) ||
      (context && !AdmitSimpleSmallCapsScript(*context, length))) {
    return false;
  }
  const auto* caps = SmallCapsForRun(registry_, fontManager_, font, FontVariant::SmallCaps, text,
                                     offset, length, context);
  return caps && !caps->empty();
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
  if (context.work &&
      !(fontVariant == FontVariant::SmallCaps ? AdmitSimpleSmallCapsScript(context, byteLength)
                                              : context.work->consume(byteLength))) {
    return {};
  }
  bool backward = false;
#ifdef DONNER_TEXT_ENABLED
  backward = context.rightToLeft.value_or(false);
#endif
  return shapeRunImpl(font, fontSizePx, spanText, byteOffset, byteLength, isVertical,
                      textOrientation, fontVariant, kerning, false, backward, &context);
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

/// Upright vertical metrics in design units, synthesized the way FreeType does for the full
/// backend so both backends agree: with vertical metrics, the vmtx advance and the top side
/// bearing above the glyph box; without them, an em cell from the OS/2 typographic (or else
/// horizontal header) ascent and descent, with the glyph box centered in it. This matches FreeType
/// for well-formed tables; truncated vmtx records are clamped, and an empty typographic cell
/// falls back to the horizontal header.
class SimpleVerticalMetrics {
public:
  /// Reads `vhea`, `vmtx` and the em cell for \p font.
  static SimpleVerticalMetrics Read(const FontManager& manager, FontHandle font,
                                    const stbtt_fontinfo* info) {
    SimpleVerticalMetrics result;
    const auto vhea = manager.sfntTable(font, "vhea");
    const auto vmtx = manager.sfntTable(font, "vmtx");
    if (vhea && vhea->size() >= 36 && vmtx) {
      const size_t declared = static_cast<uint16_t>(ReadInt16Be(*vhea, 34));
      result.longMetrics_ = std::min(declared, vmtx->size() / 4);
      if (result.longMetrics_ > 0) {
        result.vmtx_ = vmtx;
      }
    }
    if (!result.vmtx_) {
      result.fallbackAscent_ = readFallbackAscent(manager, font);
    }
    if (const auto os2 = manager.sfntTable(font, "OS/2"); os2 && os2->size() >= 78) {
      result.cell_ = ReadInt16Be(*os2, 68) - ReadInt16Be(*os2, 70);
    }
    // An empty typographic cell (zeroed OS/2 fields) falls back to the horizontal header.
    if (result.cell_ <= 0) {
      int ascent = 0;
      int descent = 0;
      int lineGap = 0;
      stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
      result.cell_ = ascent - descent;
    }
    return result;
  }

  /// Returns the vertical advance of \p glyphIndex.
  int advance(int glyphIndex) const {
    if (!vmtx_) {
      return std::abs(cell_);
    }
    const size_t index = std::min(static_cast<size_t>(glyphIndex), longMetrics_ - 1);
    return static_cast<uint16_t>(ReadInt16Be(*vmtx_, index * 4));
  }

  /**
   * Returns the vertical origin Y of a glyph; an empty glyph passes a zero-height box.
   *
   * @param glyphIndex Glyph to place.
   * @param yMin Bottom of the glyph box.
   * @param yMax Top of the glyph box.
   */
  int originY(int glyphIndex, int yMin, int yMax) const {
    if (!vmtx_) {
      // One synthesized origin keeps upright letters with different ink heights on the same
      // baseline.
      if (fallbackAscent_ > 0) {
        return fallbackAscent_;
      }
      // FreeType truncates the centered top side bearing to whole font units.
      return yMax + (cell_ - (yMax - yMin)) / 2;
    }
    const size_t index = static_cast<size_t>(glyphIndex);
    const size_t offset =
        index < longMetrics_ ? index * 4 + 2 : longMetrics_ * 4 + (index - longMetrics_) * 2;
    // Like FreeType, a glyph past the end of the bearing array has a zero top side bearing.
    return yMax + (offset + 2 <= vmtx_->size() ? ReadInt16Be(*vmtx_, offset) : 0);
  }

private:
  static int readFallbackAscent(const FontManager& manager, FontHandle font) {
    if (manager.sfntTable(font, "VORG")) {
      return 0;
    }
    const auto hhea = manager.sfntTable(font, "hhea");
    return hhea && hhea->size() >= 6 ? ReadInt16Be(*hhea, 4) : 0;
  }

  std::optional<std::span<const uint8_t>> vmtx_;
  size_t longMetrics_ = 0;  //!< Clamped `numOfLongVerMetrics`; nonzero whenever vmtx_ is set.
  int cell_ = 0;            //!< Ascent minus descent of the synthesized vertical em cell.
  int fallbackAscent_ = 0;  //!< Nonpositive when the face cannot supply a fallback from hhea.
};

/// Sets the advance axis for horizontal, sideways, or upright vertical text.
///
/// Upright glyphs, including CJK in mixed orientation, are horizontally centered on the vertical
/// origin from SimpleVerticalMetrics and advance by its vertical advance. \p uprightBase is the
/// preceding glyph when it was placed upright too (upright runs are always in logical order); it
/// anchors zero-advance marks.
void SetSimpleGlyphAdvance(TextBackend::ShapedGlyph& glyph, bool isVertical,
                           TextOrientation textOrientation, uint32_t codepoint,
                           const stbtt_fontinfo* info, const SimpleVerticalMetrics& verticalMetrics,
                           const TextBackend::ShapedGlyph* uprightBase, int glyphIndex,
                           int advanceWidth, float glyphScale) {
  const bool sideways = UsesSidewaysMetrics(textOrientation, codepoint);
  if (isVertical && sideways) {
    // Sideways Latin in vertical mode: horizontal advance becomes vertical.
    glyph.xAdvance = 0;
    glyph.yAdvance = static_cast<double>(advanceWidth) * glyphScale;
  } else if (isVertical) {
    const double cellAdvance =
        static_cast<double>(verticalMetrics.advance(glyphIndex)) * glyphScale;
    glyph.xAdvance = 0;
    glyph.yAdvance = advanceWidth == 0 ? 0.0 : cellAdvance;
    if (advanceWidth == 0 && uprightBase) {
      // A zero-advance mark is drawn in its base glyph's horizontal frame, one cell back.
      glyph.xOffset = uprightBase->xOffset;
      glyph.yOffset = uprightBase->yOffset - uprightBase->yAdvance;
      return;
    }
    int xMin = 0;
    int yMin = 0;
    int xMax = 0;
    int yMax = 0;
    if (!stbtt_GetGlyphBox(info, glyphIndex, &xMin, &yMin, &xMax, &yMax)) {
      yMin = yMax = 0;
    }
    glyph.xOffset = -static_cast<double>(advanceWidth) * glyphScale / 2.0;
    glyph.yOffset =
        static_cast<double>(verticalMetrics.originY(glyphIndex, yMin, yMax)) * glyphScale;
    if (advanceWidth == 0) {
      glyph.yOffset -= cellAdvance;
    }
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
  float scale;
  bool vertical;
  TextOrientation orientation;
  FontVariant variant;
  bool kerning;
  SimpleVerticalMetrics verticalMetrics;
  const SimpleSmallCapsSubstitution* smallCaps;

  TextBackend::ShapedGlyph glyph(SimpleScalar scalar, int previousGlyph,
                                 const TextBackend::ShapedGlyph* uprightBase) const {
    const bool nativeSmallCaps = smallCaps && !smallCaps->empty();
    // Without native small caps, lowercase ASCII is synthesized as scaled capitals.
    const bool smallCap = variant == FontVariant::SmallCaps && !nativeSmallCaps &&
                          scalar.codepoint >= 'a' && scalar.codepoint <= 'z';
    if (smallCap) {
      scalar.codepoint = scalar.codepoint - 'a' + 'A';
    }
    int index = stbtt_FindGlyphIndex(info, static_cast<int>(scalar.codepoint));
    if (nativeSmallCaps) {
      // stb_truetype does not range-check glyph metrics, so a substitute past the face is ignored.
      const int substitute = smallCaps->substitute(index);
      index = substitute < info->numGlyphs ? substitute : index;
    }
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
    SetSimpleGlyphAdvance(result, vertical, orientation, scalar.codepoint, info, verticalMetrics,
                          uprightBase, index, advanceWidth, glyphScale);
    return result;
  }
};

/// Keep upright ranges in their vertical shaping profile; sideways runs use horizontal ordering.
bool SimpleRangeUsesSidewaysComposition(std::string_view text, TextOrientation orientation) {
  if (orientation == TextOrientation::Sideways) {
    return true;
  }
  if (orientation != TextOrientation::Mixed) {
    return false;
  }
  for (size_t byte = 0; byte < text.size();) {
    if (!UsesSidewaysMetrics(orientation, decodeUtf8(text, byte))) {
      return false;
    }
  }
  return true;
}

bool ValidSimpleShapingRange(float size, std::string_view text, size_t offset, size_t length) {
  return std::isfinite(size) && size > 0 && offset <= text.size() && length <= text.size() - offset;
}

}  // namespace

TextBackend::ShapedRun TextBackendSimple::shapeRunImpl(FontHandle font, float fontSizePx,
                                                       std::string_view spanText, size_t byteOffset,
                                                       size_t byteLength, bool isVertical,
                                                       TextOrientation textOrientation,
                                                       FontVariant fontVariant, bool enableKerning,
                                                       bool /*forceLogicalOrder*/, bool backward,
                                                       const ShapingContext* context) const {
  if (!ValidSimpleShapingRange(fontSizePx, spanText, byteOffset, byteLength)) {
    return {};
  }
  if (backward && isVertical) {
    backward = SimpleRangeUsesSidewaysComposition(spanText.substr(byteOffset, byteLength),
                                                  textOrientation);
  }
  const stbtt_fontinfo* info = getFontInfo(font);
  if (!info) {
    return {};
  }
  const float scale = scaleForPixelHeight(font, fontSizePx);
  if (scale == 0) {
    return {};
  }
  const SimpleShapingMetrics metrics{
      info,
      scale,
      isVertical,
      textOrientation,
      fontVariant,
      enableKerning,
      isVertical ? SimpleVerticalMetrics::Read(fontManager_, font, info) : SimpleVerticalMetrics{},
      SmallCapsForRun(registry_, fontManager_, font, fontVariant, spanText, byteOffset, byteLength,
                      context)};
  SimpleScalarSequence sequence(spanText, byteOffset, byteLength, backward);
  ShapedRun result;
  result.backward = backward;
  int previousGlyph = 0;
  bool previousUpright = false;
  while (const auto scalar = sequence.next()) {
    result.glyphs.push_back(
        metrics.glyph(*scalar, previousGlyph, previousUpright ? &result.glyphs.back() : nullptr));
    previousGlyph = result.glyphs.back().glyphIndex;
    previousUpright = isVertical && !UsesSidewaysMetrics(textOrientation, scalar->codepoint);
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
