#include "donner/svg/resources/FontManager.h"

#include <cstring>
#include <iostream>

#include "donner/base/fonts/WoffFont.h"
#include "donner/base/fonts/WoffParser.h"
#ifdef DONNER_TEXT_WOFF2_ENABLED
#include "donner/base/fonts/Woff2Parser.h"
#endif
#include "embed_resources/PublicSansFont.h"

#define STBTT_DEF extern
#include <stb/stb_truetype.h>

namespace donner::svg {

namespace {

/// WOFF 1.0 magic: 'wOFF'
constexpr uint32_t kWoffMagic = 0x774F4646;

/// WOFF 2.0 magic: 'wOF2'
constexpr uint32_t kWoff2Magic = 0x774F4632;

/// Write a uint32_t in big-endian format.
void writeBE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[3] = static_cast<uint8_t>(v & 0xFF);
}

/// Write a uint16_t in big-endian format.
void writeBE16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}

/// Read a uint32_t in big-endian from a byte pointer.
uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/**
 * Reconstruct a flat sfnt byte stream from a WoffFont's decompressed tables.
 *
 * The sfnt format is:
 * - 4 bytes: sfnt version (flavor)
 * - 2 bytes: numTables
 * - 2 bytes: searchRange
 * - 2 bytes: entrySelector
 * - 2 bytes: rangeShift
 * - For each table: 4+4+4+4 = 16 bytes (tag, checksum, offset, length)
 * - Then the table data, each padded to 4-byte alignment
 */
std::vector<uint8_t> reconstructSfnt(const fonts::WoffFont& woff) {
  const uint16_t numTables = static_cast<uint16_t>(woff.tables.size());

  // Compute searchRange, entrySelector, rangeShift for the table directory.
  uint16_t searchRange = 1;
  uint16_t entrySelector = 0;
  while (searchRange * 2 <= numTables) {
    searchRange *= 2;
    entrySelector++;
  }
  searchRange *= 16;
  const uint16_t rangeShift = numTables * 16 - searchRange;

  // Header: 12 bytes, then 16 bytes per table directory entry.
  const size_t headerSize = 12 + static_cast<size_t>(numTables) * 16;

  // Calculate total size: header + all table data (4-byte aligned).
  size_t totalSize = headerSize;
  for (const auto& table : woff.tables) {
    totalSize += (table.data.size() + 3) & ~size_t{3};  // 4-byte align
  }

  std::vector<uint8_t> sfnt(totalSize, 0);
  uint8_t* out = sfnt.data();

  // Write sfnt header.
  writeBE32(out, woff.flavor);
  writeBE16(out + 4, numTables);
  writeBE16(out + 6, searchRange);
  writeBE16(out + 8, entrySelector);
  writeBE16(out + 10, rangeShift);

  // Write table directory entries and table data.
  uint32_t dataOffset = static_cast<uint32_t>(headerSize);
  uint8_t* dir = out + 12;

  for (const auto& table : woff.tables) {
    // Compute checksum: sum of 32-bit words (big-endian, zero-padded to 4 bytes).
    uint32_t checksum = 0;
    const size_t paddedLen = (table.data.size() + 3) & ~size_t{3};
    for (size_t i = 0; i < paddedLen; i += 4) {
      uint32_t word = 0;
      for (size_t j = 0; j < 4 && (i + j) < table.data.size(); ++j) {
        word |= static_cast<uint32_t>(table.data[i + j]) << (24 - 8 * j);
      }
      checksum += word;
    }

    writeBE32(dir, table.tag);
    writeBE32(dir + 4, checksum);
    writeBE32(dir + 8, dataOffset);
    writeBE32(dir + 12, static_cast<uint32_t>(table.data.size()));
    dir += 16;

    // Copy table data.
    std::memcpy(out + dataOffset, table.data.data(), table.data.size());
    dataOffset += static_cast<uint32_t>(paddedLen);
  }

  return sfnt;
}

}  // namespace

struct FontManager::LoadedFont {
  std::vector<uint8_t> data;  // Owns the font file bytes (stb_truetype references them).
  stbtt_fontinfo info{};      // stb_truetype parsed font handle.
};

FontManager::FontManager() = default;
FontManager::~FontManager() = default;

void FontManager::addFontFace(const css::FontFace& face) {
  faces_.push_back(face);
}

FontHandle FontManager::findFont(std::string_view family) {
  // Check cache first.
  const std::string familyStr(family);
  if (auto it = cache_.find(familyStr); it != cache_.end()) {
    return it->second;
  }

  // Walk @font-face rules looking for a matching family.
  for (const auto& face : faces_) {
    if (face.familyName != family) {
      continue;
    }

    for (const auto& source : face.sources) {
      FontHandle handle;

      if (source.kind == css::FontFaceSource::Kind::Data) {
        const auto& data = std::get<std::vector<uint8_t>>(source.payload);
        handle = loadFontData(data);
      }
      // Note: Kind::Url and Kind::Local are not handled here yet.
      // URL loading requires a ResourceLoaderInterface which we'll integrate in a future phase.
      // Local system fonts are not supported for TinySkia (no system font access).

      if (handle) {
        cache_[familyStr] = handle;
        return handle;
      }
    }
  }

  // Fall back to the embedded Public Sans font.
  FontHandle fb = fallbackFont();
  cache_[familyStr] = fb;
  return fb;
}

FontHandle FontManager::loadFontData(std::span<const uint8_t> data) {
  if (data.size() < 4) {
    return FontHandle();
  }

  const uint32_t magic = readBE32(data.data());

  if (magic == kWoffMagic) {
    return loadWoff1(data);
  }

  if (magic == kWoff2Magic) {
#ifdef DONNER_TEXT_WOFF2_ENABLED
    return loadWoff2(data);
#else
    std::cerr << "FontManager: WOFF2 font encountered but WOFF2 support not enabled. "
                 "Build with --config=text-woff2 to enable.\n";
    return FontHandle();
#endif
  }

  // Treat as raw TTF/OTF. Copy the data since stb_truetype holds a pointer.
  std::vector<uint8_t> owned(data.begin(), data.end());
  return loadRawFont(std::move(owned));
}

const stbtt_fontinfo* FontManager::fontInfo(FontHandle handle) const {
  if (!handle || handle.index() < 0 ||
      static_cast<size_t>(handle.index()) >= fonts_.size()) {
    return nullptr;
  }
  return &fonts_[static_cast<size_t>(handle.index())]->info;
}

float FontManager::scaleForPixelHeight(FontHandle handle, float pixelHeight) const {
  const stbtt_fontinfo* info = fontInfo(handle);
  if (!info) {
    return 0.0f;
  }
  return stbtt_ScaleForPixelHeight(info, pixelHeight);
}

std::span<const uint8_t> FontManager::fontData(FontHandle handle) const {
  if (!handle || handle.index() < 0 ||
      static_cast<size_t>(handle.index()) >= fonts_.size()) {
    return {};
  }
  const auto& font = fonts_[static_cast<size_t>(handle.index())];
  return {font->data.data(), font->data.size()};
}

FontHandle FontManager::fallbackFont() {
  if (fallbackHandle_) {
    return fallbackHandle_;
  }

  // Load the embedded Public Sans font.
  std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                            embedded::kPublicSansMediumOtf.end());
  fallbackHandle_ = loadRawFont(std::move(data));
  if (!fallbackHandle_) {
    std::cerr << "FontManager: Failed to load embedded fallback font (Public Sans)\n";
  }
  return fallbackHandle_;
}

FontHandle FontManager::loadRawFont(std::vector<uint8_t> data) {
  auto font = std::make_unique<LoadedFont>();
  font->data = std::move(data);

  if (!stbtt_InitFont(&font->info, font->data.data(), 0)) {
    std::cerr << "FontManager: stbtt_InitFont failed for raw font data\n";
    return FontHandle();
  }

  const int index = static_cast<int>(fonts_.size());
  fonts_.push_back(std::move(font));
  return FontHandle(index);
}

FontHandle FontManager::loadWoff1(std::span<const uint8_t> data) {
  auto maybeFont = fonts::WoffParser::Parse(data);
  if (maybeFont.hasError()) {
    std::cerr << "FontManager: WOFF1 parsing failed: " << maybeFont.error().reason << "\n";
    return FontHandle();
  }

  // Reconstruct sfnt byte stream from decompressed WOFF tables.
  std::vector<uint8_t> sfntData = reconstructSfnt(maybeFont.result());
  return loadRawFont(std::move(sfntData));
}

#ifdef DONNER_TEXT_WOFF2_ENABLED
FontHandle FontManager::loadWoff2(std::span<const uint8_t> data) {
  auto result = fonts::Woff2Parser::Decompress(data);
  if (result.hasError()) {
    std::cerr << "FontManager: WOFF2 decompression failed: " << result.error().reason << "\n";
    return FontHandle();
  }

  return loadRawFont(std::move(result.result()));
}
#endif

}  // namespace donner::svg
