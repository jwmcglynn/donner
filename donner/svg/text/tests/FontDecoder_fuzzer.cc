#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/fonts/SfntUtils.h"
#ifdef DONNER_TEXT_FULL
#include "donner/svg/text/TextBackendFull.h"
#else
#include "donner/svg/text/TextBackendSimple.h"
#endif
#include "donner/svg/core/FontVariant.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::svg {
namespace {

#ifdef DONNER_TEXT_FULL
using FuzzTextBackend = TextBackendFull;
#else
using FuzzTextBackend = TextBackendSimple;
#endif

void WriteBe16(std::vector<uint8_t>* data, size_t offset, uint16_t value) {
  (*data)[offset] = static_cast<uint8_t>(value >> 8);
  (*data)[offset + 1] = static_cast<uint8_t>(value);
}

void WriteBe32(std::vector<uint8_t>* data, size_t offset, uint32_t value) {
  (*data)[offset] = static_cast<uint8_t>(value >> 24);
  (*data)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*data)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*data)[offset + 3] = static_cast<uint8_t>(value);
}

struct TableSpec {
  std::string_view tag;
  std::vector<uint8_t> bytes;
};

std::vector<uint8_t> MakeSfnt(std::vector<TableSpec> tables) {
  const size_t directorySize = 12 + tables.size() * 16;
  std::vector<uint8_t> result(directorySize, 0);
  WriteBe32(&result, 0, 0x00010000);
  WriteBe16(&result, 4, static_cast<uint16_t>(tables.size()));

  size_t dataOffset = directorySize;
  for (size_t i = 0; i < tables.size(); ++i) {
    dataOffset = (dataOffset + 3) & ~size_t{3};
    result.resize(dataOffset + tables[i].bytes.size());
    const size_t recordOffset = 12 + i * 16;
    for (size_t j = 0; j < 4; ++j) {
      result[recordOffset + j] = static_cast<uint8_t>(tables[i].tag[j]);
    }
    WriteBe32(&result, recordOffset + 8, static_cast<uint32_t>(dataOffset));
    WriteBe32(&result, recordOffset + 12, static_cast<uint32_t>(tables[i].bytes.size()));
    for (size_t j = 0; j < tables[i].bytes.size(); ++j) {
      result[dataOffset + j] = tables[i].bytes[j];
    }
    dataOffset += tables[i].bytes.size();
  }
  return result;
}

std::vector<uint8_t> CompoundGlyph(std::span<const uint16_t> dependencies) {
  std::vector<uint8_t> glyph(10, 0);
  WriteBe16(&glyph, 0, 0xFFFF);
  for (size_t i = 0; i < dependencies.size(); ++i) {
    const size_t offset = glyph.size();
    glyph.resize(offset + 6);
    WriteBe16(&glyph, offset,
              static_cast<uint16_t>(0x0002 | (i + 1 < dependencies.size() ? 0x0020 : 0)));
    WriteBe16(&glyph, offset + 2, dependencies[i]);
  }
  return glyph;
}

std::vector<uint8_t> EmptyCmap() {
  std::vector<uint8_t> cmap(28, 0);
  WriteBe16(&cmap, 2, 1);
  WriteBe16(&cmap, 4, 3);
  WriteBe16(&cmap, 6, 10);
  WriteBe32(&cmap, 8, 12);
  WriteBe16(&cmap, 12, 12);
  WriteBe32(&cmap, 16, 16);
  return cmap;
}

std::vector<uint8_t> MakeTrueType(std::vector<std::vector<uint8_t>> glyphs) {
  std::vector<uint8_t> head(54, 0);
  WriteBe16(&head, 18, 1000);
  WriteBe16(&head, 50, 1);
  std::vector<uint8_t> maxp(6, 0);
  WriteBe32(&maxp, 0, 0x00005000);
  WriteBe16(&maxp, 4, static_cast<uint16_t>(glyphs.size()));
  std::vector<uint8_t> hhea(36, 0);
  WriteBe16(&hhea, 4, 800);
  WriteBe16(&hhea, 6, static_cast<uint16_t>(-200));
  WriteBe16(&hhea, 34, static_cast<uint16_t>(glyphs.size()));
  std::vector<uint8_t> hmtx(glyphs.size() * 4, 0);

  std::vector<uint8_t> loca((glyphs.size() + 1) * 4, 0);
  std::vector<uint8_t> glyf;
  for (size_t i = 0; i < glyphs.size(); ++i) {
    WriteBe32(&loca, i * 4, static_cast<uint32_t>(glyf.size()));
    glyf.insert(glyf.end(), glyphs[i].begin(), glyphs[i].end());
  }
  WriteBe32(&loca, glyphs.size() * 4, static_cast<uint32_t>(glyf.size()));

  return MakeSfnt({{"cmap", EmptyCmap()},
                   {"glyf", std::move(glyf)},
                   {"head", std::move(head)},
                   {"hhea", std::move(hhea)},
                   {"hmtx", std::move(hmtx)},
                   {"loca", std::move(loca)},
                   {"maxp", std::move(maxp)}});
}

std::vector<uint8_t> MakeDependencyChain(size_t depth) {
  std::vector<std::vector<uint8_t>> glyphs(depth, std::vector<uint8_t>(10, 0));
  for (size_t i = 0; i + 1 < depth; ++i) {
    const uint16_t child = static_cast<uint16_t>(i + 1);
    glyphs[i] = CompoundGlyph({&child, 1});
  }
  return MakeTrueType(std::move(glyphs));
}

std::vector<uint8_t> MakeStructuredFont(uint8_t selector) {
  switch (selector) {
    case 'S': {
      const uint16_t self = 0;
      return MakeTrueType({CompoundGlyph({&self, 1})});
    }
    case 'D': return MakeDependencyChain(fonts::kMaximumCompoundGlyphDepth);
    case 'E': return MakeDependencyChain(fonts::kMaximumCompoundGlyphDepth + 1);
    case 'W': {
      std::vector<uint16_t> dependencies(fonts::kMaximumCompoundComponentRecords + 1, 1);
      return MakeTrueType({CompoundGlyph(dependencies), std::vector<uint8_t>(10, 0)});
    }
    case 'T': {
      std::vector<uint8_t> data(12 + (fonts::kMaximumSfntTables + 1) * 16, 0);
      WriteBe32(&data, 0, 0x00010000);
      WriteBe16(&data, 4, static_cast<uint16_t>(fonts::kMaximumSfntTables + 1));
      return data;
    }
    case 'A': return MakeDependencyChain(1);
    default: return {};
  }
}

void ExerciseLoadedFont(FontManager& fontManager, Registry& registry, FontHandle font) {
  FuzzTextBackend backend(fontManager, registry);
  (void)backend.fontVMetrics(font);
  (void)backend.scaleForPixelHeight(font, 16.0f);
  (void)backend.scaleForEmToPixels(font, 16.0f);
  (void)backend.underlineMetrics(font);
  (void)backend.strikeoutMetrics(font);
  (void)backend.subSuperMetrics(font);
  (void)backend.isBitmapOnly(font);
  (void)backend.hasSmallCapsFeature(font);

  // Exercise the compound dependency rooted at glyph 0 even when cmap has no mapping for it.
  (void)backend.glyphOutline(font, 0, 1.0f);

  constexpr std::string_view kText = "Az";
  const TextBackend::ShapedRun shaped =
      backend.shapeRun(font, 16.0f, kText, 0, kText.size(), false, FontVariant::Normal, false);
  for (const TextBackend::ShapedGlyph& glyph : shaped.glyphs) {
    (void)backend.glyphOutline(font, glyph.glyphIndex, 1.0f);
  }
  (void)backend.crossSpanKern(font, 16.0f, font, 16.0f, 'A', 'z', false);
}

void ExerciseAccounting(std::span<const uint8_t> fontBytes) {
  auto sfnt = fonts::SfntFont::Validate(fontBytes);
  if (!sfnt) {
    return;
  }
  const size_t charge = fontBytes.size() + sfnt->retainedBytes();
  Registry registry;
  FontManager fontManager(registry, charge, 1);
  const FontHandle first = fontManager.loadFontData(fontBytes);
  if (!first) {
    return;
  }
  (void)fontManager.loadFontData(fontBytes);
  registry.destroy(first.entity());
  const FontHandle replacement = fontManager.loadFontData(fontBytes);
  if (replacement) {
    ExerciseLoadedFont(fontManager, registry, replacement);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Single-byte corpus seeds select deterministic, trusted synthetic fonts. Every other input is
  // passed through unchanged as an untrusted document-provided font.
  std::vector<uint8_t> structured;
  std::span<const uint8_t> fontBytes(data, size);
  const bool isStructuredSeed = size == 1 || (size == 2 && data[1] == '\n');
  if (isStructuredSeed) {
    structured = MakeStructuredFont(data[0]);
    if (!structured.empty()) {
      fontBytes = structured;
    }
  }

  if (isStructuredSeed && data[0] == 'A') {
    ExerciseAccounting(fontBytes);
    return 0;
  }

  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = fontManager.loadFontData(fontBytes);
  if (font) {
    ExerciseLoadedFont(fontManager, registry, font);
  }
  return 0;
}

}  // namespace donner::svg
