#include <algorithm>
#include <array>
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

std::vector<uint8_t> MakeSfnt(std::vector<TableSpec> tables, uint32_t magic = 0x00010000) {
  const size_t directorySize = 12 + tables.size() * 16;
  std::vector<uint8_t> result(directorySize, 0);
  WriteBe32(&result, 0, magic);
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

std::vector<uint8_t> SimpleGlyph(size_t pointCount) {
  std::vector<uint8_t> glyph(14, 0);
  WriteBe16(&glyph, 0, 1);
  WriteBe16(&glyph, 10, static_cast<uint16_t>(pointCount - 1));
  size_t remaining = pointCount;
  while (remaining != 0) {
    const size_t runLength = std::min(remaining, size_t{256});
    glyph.push_back(runLength == 1 ? 0x31 : 0x39);
    if (runLength != 1) {
      glyph.push_back(static_cast<uint8_t>(runLength - 1));
    }
    remaining -= runLength;
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

struct StructuredFont {
  std::vector<uint8_t> bytes;
  int outlineGlyph = 0;
};

StructuredFont MakeSharedTail(bool appendTooDeepRoot) {
  const size_t tailDepth = fonts::kMaximumCompoundGlyphDepth - 1;
  std::vector<std::vector<uint8_t>> glyphs(tailDepth, std::vector<uint8_t>(10, 0));
  glyphs.back() = SimpleGlyph(1);
  for (size_t i = 0; i + 1 < tailDepth; ++i) {
    const uint16_t child = static_cast<uint16_t>(i + 1);
    glyphs[i] = CompoundGlyph({&child, 1});
  }

  const uint16_t tail = 0;
  glyphs.push_back(CompoundGlyph({&tail, 1}));
  glyphs.push_back(CompoundGlyph({&tail, 1}));
  int outlineGlyph = static_cast<int>(glyphs.size() - 1);
  if (appendTooDeepRoot) {
    const uint16_t completedDepth32Root = static_cast<uint16_t>(tailDepth);
    glyphs.push_back(CompoundGlyph({&completedDepth32Root, 1}));
    outlineGlyph = static_cast<int>(glyphs.size() - 1);
  }
  return {MakeTrueType(std::move(glyphs)), outlineGlyph};
}

uint64_t RepeatedComponentWork(size_t componentCount, size_t childPoints) {
  const uint64_t childVertices = childPoints + 2;
  const uint64_t childWork = childVertices + 4 * childPoints;
  const uint64_t components = componentCount;
  return 1 + components * (childWork + childVertices + 1) +
         childVertices * components * (components + 1) / 2;
}

StructuredFont MakeRepeatedWorkFont(bool aboveCap) {
  constexpr size_t kChildPoints = 64;
  size_t componentCount = 0;
  while (componentCount < fonts::kMaximumCompoundComponentRecords &&
         RepeatedComponentWork(componentCount + 1, kChildPoints) <=
             fonts::kMaximumGlyphOutlineWork) {
    ++componentCount;
  }
  componentCount += aboveCap ? 1 : 0;
  std::vector<uint16_t> dependencies(componentCount, 1);
  return {MakeTrueType({CompoundGlyph(dependencies), SimpleGlyph(kChildPoints)}), 0};
}

StructuredFont MakeNestedSharedWorkFont(bool aboveCap) {
  constexpr size_t kLeafPoints = 64;
  std::vector<std::vector<uint8_t>> glyphs{SimpleGlyph(kLeafPoints)};
  uint64_t vertices = kLeafPoints + 2;
  uint64_t work = vertices + 4 * kLeafPoints;
  while (glyphs.size() < fonts::kMaximumCompoundGlyphDepth) {
    const uint64_t nextVertices = vertices * 2;
    const uint64_t nextWork = 2 * work + 5 * vertices + 3;
    if (nextVertices > fonts::kMaximumExpandedGlyphVertices ||
        nextWork > fonts::kMaximumGlyphOutlineWork) {
      break;
    }
    const uint16_t sharedChild = static_cast<uint16_t>(glyphs.size() - 1);
    const std::array<uint16_t, 2> dependencies{sharedChild, sharedChild};
    glyphs.push_back(CompoundGlyph(dependencies));
    vertices = nextVertices;
    work = nextWork;
  }
  if (aboveCap) {
    const uint16_t sharedChild = static_cast<uint16_t>(glyphs.size() - 1);
    const std::array<uint16_t, 2> dependencies{sharedChild, sharedChild};
    glyphs.push_back(CompoundGlyph(dependencies));
  }
  const int outlineGlyph = static_cast<int>(glyphs.size() - 1);
  return {MakeTrueType(std::move(glyphs)), outlineGlyph};
}

StructuredFont MakeMalformedFinalCff() {
  std::vector<uint8_t> head(54, 0);
  WriteBe16(&head, 18, 1000);
  std::vector<uint8_t> hhea(36, 0);
  WriteBe16(&hhea, 4, 800);
  WriteBe16(&hhea, 6, static_cast<uint16_t>(-200));
  WriteBe16(&hhea, 34, 1);
  std::vector<uint8_t> maxp(6, 0);
  WriteBe16(&maxp, 4, 1);
  return {MakeSfnt({{"cmap", EmptyCmap()},
                    {"head", std::move(head)},
                    {"hhea", std::move(hhea)},
                    {"hmtx", std::vector<uint8_t>(4, 0)},
                    {"maxp", std::move(maxp)},
                    {"CFF ", {1, 0, 4, 4}}},
                   0x4F54544F),
          0};
}

StructuredFont MakeStructuredFont(uint8_t selector) {
  switch (selector) {
    case 'S': {
      const uint16_t self = 0;
      return {MakeTrueType({CompoundGlyph({&self, 1})}), 0};
    }
    case 'D': return {MakeDependencyChain(fonts::kMaximumCompoundGlyphDepth), 0};
    case 'E': return {MakeDependencyChain(fonts::kMaximumCompoundGlyphDepth + 1), 0};
    case 'W': {
      std::vector<uint16_t> dependencies(fonts::kMaximumCompoundComponentRecords + 1, 1);
      return {MakeTrueType({CompoundGlyph(dependencies), std::vector<uint8_t>(10, 0)}), 0};
    }
    case 'T': {
      std::vector<uint8_t> data(12 + (fonts::kMaximumSfntTables + 1) * 16, 0);
      WriteBe32(&data, 0, 0x00010000);
      WriteBe16(&data, 4, static_cast<uint16_t>(fonts::kMaximumSfntTables + 1));
      return {std::move(data), 0};
    }
    case 'A': return {MakeDependencyChain(1), 0};
    case 'H': return MakeSharedTail(false);
    case 'J': return MakeSharedTail(true);
    case 'R': return MakeRepeatedWorkFont(false);
    case 'X': return MakeRepeatedWorkFont(true);
    case 'M': return MakeNestedSharedWorkFont(false);
    case 'N': return MakeNestedSharedWorkFont(true);
    default: return {};
  }
}

void ExerciseLoadedFont(FontManager& fontManager, Registry& registry, FontHandle font,
                        int outlineGlyph = 0) {
  FuzzTextBackend backend(fontManager, registry);
  (void)backend.fontVMetrics(font);
  (void)backend.scaleForPixelHeight(font, 16.0f);
  (void)backend.scaleForEmToPixels(font, 16.0f);
  (void)backend.underlineMetrics(font);
  (void)backend.strikeoutMetrics(font);
  (void)backend.subSuperMetrics(font);
  (void)backend.isBitmapOnly(font);
  (void)backend.hasSmallCapsFeature(font);

  // Exercise the selected compound root directly even when cmap has no mapping for it. The
  // shared-tail seed deliberately selects a high glyph index at the exact depth-32 boundary.
  (void)backend.glyphOutline(font, outlineGlyph, 1.0f);

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
  const FontHandle first = fontManager.loadFontData(fontBytes, FontDataTrust::Trusted);
  if (!first) {
    return;
  }
  (void)fontManager.loadFontData(fontBytes, FontDataTrust::Trusted);
  registry.destroy(first.entity());
  const FontHandle replacement = fontManager.loadFontData(fontBytes, FontDataTrust::Trusted);
  if (replacement) {
    ExerciseLoadedFont(fontManager, registry, replacement);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Single-byte corpus seeds select trusted synthetic fonts. The malformed CFF sentinel and every
  // arbitrary input remain untrusted document-equivalent bytes.
  constexpr std::string_view kMalformedFinalCffSeed = "MALFORMED_FINAL_CFF\n";
  StructuredFont structured;
  std::span<const uint8_t> fontBytes(data, size);
  FontDataTrust trust = FontDataTrust::Untrusted;
  const bool isStructuredSeed = size == 1 || (size == 2 && data[1] == '\n');
  const bool isMalformedFinalCffSeed =
      size == kMalformedFinalCffSeed.size() &&
      std::equal(kMalformedFinalCffSeed.begin(), kMalformedFinalCffSeed.end(), data);
  if (isMalformedFinalCffSeed) {
    structured = MakeMalformedFinalCff();
    fontBytes = structured.bytes;
  } else if (isStructuredSeed) {
    structured = MakeStructuredFont(data[0]);
    if (!structured.bytes.empty()) {
      fontBytes = structured.bytes;
      trust = FontDataTrust::Trusted;
    }
  }

  if (isStructuredSeed && data[0] == 'A') {
    ExerciseAccounting(fontBytes);
    return 0;
  }

  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = fontManager.loadFontData(fontBytes, trust);
  if (font) {
    ExerciseLoadedFont(fontManager, registry, font, structured.outlineGlyph);
  }
  return 0;
}

}  // namespace donner::svg
