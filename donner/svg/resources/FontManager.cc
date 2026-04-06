#include "donner/svg/resources/FontManager.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

#include "donner/base/StringUtils.h"
#include "donner/base/fonts/WoffFont.h"
#include "donner/base/fonts/WoffParser.h"
#ifdef DONNER_TEXT_WOFF2_ENABLED
#include "donner/base/fonts/Woff2Parser.h"
#endif
#include "embed_resources/PublicSansFont.h"

namespace donner::svg {

namespace {

/// WOFF 1.0 magic: 'wOFF'
constexpr uint32_t kWoffMagic = 0x774F4646;

/// WOFF 2.0 magic: 'wOF2'
constexpr uint32_t kWoff2Magic = 0x774F4632;
constexpr uint32_t kSfntTrueType = 0x00010000;
constexpr uint32_t kSfntCff = 0x4F54544F;    // "OTTO"
constexpr uint32_t kSfntApple = 0x74727565;  // "true"
constexpr uint32_t kSfntType1 = 0x74797031;  // "typ1"

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

struct FontManager::FontFaceComponent {
  explicit FontFaceComponent(css::FontFace fontFace) : face(std::move(fontFace)) {}

  css::FontFace face;
};

struct FontManager::LoadedFontComponent {
  std::vector<uint8_t> ownedData;                          // Owns reconstructed sfnt bytes.
  std::shared_ptr<const std::vector<uint8_t>> sharedData;  // Shares raw TTF/OTF bytes.

  std::span<const uint8_t> fontData() const {
    if (sharedData) {
      return {sharedData->data(), sharedData->size()};
    }

    return {ownedData.data(), ownedData.size()};
  }
};

FontManager::FontManager(Registry& registry) : registry_(registry) {}
FontManager::~FontManager() = default;

void FontManager::addFontFace(const css::FontFace& face) {
  const Entity entity = registry_.create();
  registry_.emplace<FontFaceComponent>(entity, face);
  cache_.clear();
}

size_t FontManager::numFaces() const {
  size_t count = 0;
  for (const Entity entity : registry_.view<FontFaceComponent>()) {
    static_cast<void>(entity);
    ++count;
  }
  return count;
}

std::string_view FontManager::faceFamilyName(size_t index) const {
  size_t currentIndex = 0;
  auto view = registry_.view<FontFaceComponent>();
  for (const Entity entity : view) {
    if (currentIndex == index) {
      return std::string_view(view.get<FontFaceComponent>(entity).face.familyName);
    }
    ++currentIndex;
  }

  return {};
}

void FontManager::setGenericFamilyMapping(std::string_view genericName,
                                          std::string_view realFamily) {
  std::string key(genericName);
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  genericFamilyMap_[std::move(key)] = std::string(realFamily);
}

FontHandle FontManager::findFont(std::string_view family) {
  return findFont(family, 400);
}

FontHandle FontManager::findFont(std::string_view family, int weight) {
  return findFont(family, weight, 0, 5);
}

FontHandle FontManager::findFont(std::string_view family, int weight, int style, int stretch) {
  // Resolve CSS generic family names to real family names.
  std::string familyLower(family);
  std::transform(familyLower.begin(), familyLower.end(), familyLower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (auto it = genericFamilyMap_.find(familyLower); it != genericFamilyMap_.end()) {
    family = it->second;
  }

  const std::string cacheKey = std::string(family) + ":" + std::to_string(weight) + ":" +
                               std::to_string(style) + ":" + std::to_string(stretch);
  if (auto it = cache_.find(cacheKey); it != cache_.end()) {
    return it->second;
  }

  // Walk @font-face rules looking for the best match.
  // CSS font matching: style first, then stretch, then weight.
  Entity bestEntity = entt::null;
  int bestScore = std::numeric_limits<int>::max();

  auto view = registry_.view<FontFaceComponent>();
  for (const Entity entity : view) {
    const css::FontFace& face = view.get<FontFaceComponent>(entity).face;
    if (!StringUtils::Equals<StringComparison::IgnoreCase>(face.familyName, family)) {
      continue;
    }
    // Style mismatch is most costly, then stretch, then weight.
    // CSS Fonts §5.2: oblique falls back to italic and vice versa before falling back to normal.
    int score = 0;
    if (face.fontStyle != style) {
      // Oblique (2) ↔ Italic (1) is a better match than either ↔ Normal (0).
      if ((face.fontStyle == 1 && style == 2) || (face.fontStyle == 2 && style == 1)) {
        score += 5000;  // Partial match: italic ↔ oblique.
      } else {
        score += 10000;  // Full mismatch: to/from normal.
      }
    }

    // CSS font matching for stretch: prefer faces on the correct side of the request.
    // If requesting narrower than normal (stretch < 5), prefer narrower faces.
    // If requesting wider than normal (stretch > 5), prefer wider faces.
    const int stretchDelta = face.fontStretch - stretch;
    if (stretchDelta == 0) {
      // Exact match, no penalty.
    } else if (stretch < 5 && stretchDelta < 0) {
      // Request is narrow, face is even narrower — preferred direction.
      score += (-stretchDelta) * 100;
    } else if (stretch > 5 && stretchDelta > 0) {
      // Request is wide, face is even wider — preferred direction.
      score += stretchDelta * 100;
    } else {
      // Face is on the wrong side — heavy penalty.
      score += std::abs(stretchDelta) * 100 + 1000;
    }

    score += std::abs(face.fontWeight - weight);

    if (score < bestScore) {
      bestScore = score;
      bestEntity = entity;
      if (score == 0) {
        break;  // Exact match.
      }
    }
  }

  if (bestEntity != entt::null) {
    if (registry_.all_of<LoadedFontComponent>(bestEntity)) {
      FontHandle handle(bestEntity);
      cache_[cacheKey] = handle;
      return handle;
    }

    const auto& face = registry_.get<FontFaceComponent>(bestEntity).face;
    for (const auto& source : face.sources) {
      if (source.kind == css::FontFaceSource::Kind::Data) {
        const auto& dataPtr = std::get<std::shared_ptr<const std::vector<uint8_t>>>(source.payload);
        if (loadFontDataSharedIntoEntity(bestEntity, dataPtr)) {
          FontHandle handle(bestEntity);
          cache_[cacheKey] = handle;
          return handle;
        }
      }
    }
  }

  // Fall back to the embedded Public Sans font.
  FontHandle fallback = fallbackFont();
  cache_[cacheKey] = fallback;
  return fallback;
}

FontHandle FontManager::loadFontData(std::span<const uint8_t> data) {
  const Entity entity = registry_.create();
  if (!loadFontDataIntoEntity(entity, data)) {
    registry_.destroy(entity);
    return FontHandle();
  }

  return FontHandle(entity);
}

bool FontManager::loadFontDataSharedIntoEntity(
    Entity entity, const std::shared_ptr<const std::vector<uint8_t>>& data) {
  if (!data) {
    return false;
  }

  if (data->size() < 4) {
    return false;
  }

  const uint32_t magic = readBE32(data->data());

  // WOFF fonts need decompression/reconstruction, so they create new owned buffers.
  if (magic == kWoffMagic) {
    return loadWoff1(entity, *data);
  }

  if (magic == kWoff2Magic) {
#ifdef DONNER_TEXT_WOFF2_ENABLED
    return loadWoff2(entity, *data);
#else
    std::cerr << "FontManager: WOFF2 font encountered but WOFF2 support not enabled. "
                 "Build with --config=text-full to enable.\n";
    return false;
#endif
  }

  if (magic != kSfntTrueType && magic != kSfntCff && magic != kSfntApple && magic != kSfntType1) {
    return false;
  }

  // Raw TTF/OTF: share the data via shared_ptr (no copy).
  return setRawFontData(entity, data);
}

std::span<const uint8_t> FontManager::fontData(FontHandle handle) const {
  if (!isValidHandle(handle)) {
    return {};
  }

  const auto* font = registry_.try_get<LoadedFontComponent>(handle.entity());
  return font ? font->fontData() : std::span<const uint8_t>();
}

FontHandle FontManager::fallbackFont() {
  if (isValidHandle(fallbackHandle_)) {
    return fallbackHandle_;
  }

  const Entity entity = registry_.create();

  // Load the embedded Public Sans font.
  std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                            embedded::kPublicSansMediumOtf.end());
  if (!setRawFontData(entity, std::move(data))) {
    registry_.destroy(entity);
    std::cerr << "FontManager: Failed to load embedded fallback font (Public Sans)\n";
    return FontHandle();
  }

  fallbackHandle_ = FontHandle(entity);
  return fallbackHandle_;
}

bool FontManager::setRawFontData(Entity entity, std::vector<uint8_t> data) {
  LoadedFontComponent font;
  font.ownedData = std::move(data);
  registry_.emplace_or_replace<LoadedFontComponent>(entity, std::move(font));
  return true;
}

bool FontManager::setRawFontData(Entity entity,
                                 std::shared_ptr<const std::vector<uint8_t>> sharedData) {
  if (!sharedData) {
    return false;
  }

  LoadedFontComponent font;
  font.sharedData = std::move(sharedData);
  registry_.emplace_or_replace<LoadedFontComponent>(entity, std::move(font));
  return true;
}

bool FontManager::loadWoff1(Entity entity, std::span<const uint8_t> data) {
  auto maybeFont = fonts::WoffParser::Parse(data);
  if (maybeFont.hasError()) {
    std::cerr << "FontManager: WOFF1 parsing failed: " << maybeFont.error().reason << "\n";
    return false;
  }

  // Reconstruct sfnt byte stream from decompressed WOFF tables.
  std::vector<uint8_t> sfntData = reconstructSfnt(maybeFont.result());
  return setRawFontData(entity, std::move(sfntData));
}

bool FontManager::loadFontDataIntoEntity(Entity entity, std::span<const uint8_t> data) {
  if (data.size() < 4) {
    return false;
  }

  const uint32_t magic = readBE32(data.data());

  if (magic == kWoffMagic) {
    return loadWoff1(entity, data);
  }

  if (magic == kWoff2Magic) {
#ifdef DONNER_TEXT_WOFF2_ENABLED
    return loadWoff2(entity, data);
#else
    std::cerr << "FontManager: WOFF2 font encountered but WOFF2 support not enabled. "
                 "Build with --config=text-full to enable.\n";
    return false;
#endif
  }

  if (magic != kSfntTrueType && magic != kSfntCff && magic != kSfntApple && magic != kSfntType1) {
    return false;
  }

  // Treat as raw TTF/OTF.
  std::vector<uint8_t> owned(data.begin(), data.end());
  return setRawFontData(entity, std::move(owned));
}

#ifdef DONNER_TEXT_WOFF2_ENABLED
bool FontManager::loadWoff2(Entity entity, std::span<const uint8_t> data) {
  auto result = fonts::Woff2Parser::Decompress(data);
  if (result.hasError()) {
    std::cerr << "FontManager: WOFF2 decompression failed: " << result.error().reason << "\n";
    return false;
  }

  return setRawFontData(entity, std::move(result.result()));
}
#endif

bool FontManager::isValidHandle(FontHandle handle) const {
  return handle && registry_.valid(handle.entity());
}

}  // namespace donner::svg
