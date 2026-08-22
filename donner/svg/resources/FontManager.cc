#include "donner/svg/resources/FontManager.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "donner/base/StringUtils.h"
#include "donner/base/fonts/SfntUtils.h"
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

/// ASCII-only lowercase, used for the map keys that stand in for the case-insensitive family
/// comparison. Locale-sensitive lowercasing would make those keys depend on the process locale
/// while the comparison they mirror would not.
std::string ToLowerAscii(std::string_view value) {
  std::string lowered(value);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return lowered;
}

/// Read a uint32_t in big-endian from a byte pointer.
uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool HasCffFlavor(std::span<const uint8_t> data) {
  if (data.size() < 4) return false;
  const uint32_t magic = readBE32(data.data());
  if (magic == kSfntCff) return true;
  return (magic == kWoffMagic || magic == kWoff2Magic) && data.size() >= 8 &&
         readBE32(data.data() + 4) == kSfntCff;
}

bool IsSupportedFontMagic(uint32_t magic) {
  return magic == kWoffMagic || magic == kWoff2Magic || magic == kSfntTrueType ||
         magic == kSfntCff || magic == kSfntApple || magic == kSfntType1;
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
  FontFaceComponent(css::FontFace fontFace, uint64_t declarationSequence)
      : face(std::move(fontFace)), sequence(declarationSequence) {}

  css::FontFace face;

  /// Registration order. CSS resolves a tie between equally good faces in favour of the one
  /// declared last, and storage order is not a contract, so the winner is chosen on this instead.
  uint64_t sequence = 0;
};

struct FontManager::FontBudgetState {
  size_t maximumBytes = 0;
  size_t maximumFonts = 0;
  size_t maximumValidationWork = 0;
  size_t usedBytes = 0;
  size_t usedFonts = 0;
  size_t usedValidationWork = 0;
  std::unordered_map<const std::vector<uint8_t>*, std::weak_ptr<const std::vector<uint8_t>>>
      validationRejectedSources;
};

struct FontManager::FontBudgetContext {
  std::shared_ptr<FontBudgetState> state;
};

struct FontManager::FontBudgetReservation {
  FontBudgetReservation() = default;

  FontBudgetReservation(std::shared_ptr<FontBudgetState> budgetState, size_t byteCount)
      : state(std::move(budgetState)), bytes(byteCount) {
    assert(state);
    assert(bytes <= state->maximumBytes - state->usedBytes);
    assert(state->usedFonts < state->maximumFonts);
    state->usedBytes += bytes;
    ++state->usedFonts;
  }

  FontBudgetReservation(const FontBudgetReservation&) = delete;
  FontBudgetReservation& operator=(const FontBudgetReservation&) = delete;

  FontBudgetReservation(FontBudgetReservation&& other) noexcept
      : state(std::move(other.state)), bytes(std::exchange(other.bytes, 0)) {}

  FontBudgetReservation& operator=(FontBudgetReservation&& other) noexcept {
    if (this != &other) {
      release();
      state = std::move(other.state);
      bytes = std::exchange(other.bytes, 0);
    }
    return *this;
  }

  ~FontBudgetReservation() { release(); }

  void release() {
    if (!state) {
      return;
    }
    assert(state->usedBytes >= bytes);
    assert(state->usedFonts != 0);
    state->usedBytes -= bytes;
    --state->usedFonts;
    state.reset();
    bytes = 0;
  }

  std::shared_ptr<FontBudgetState> state;
  size_t bytes = 0;
};

struct FontManager::LoadedFontComponent {
  std::vector<uint8_t> ownedData;                          // Owns reconstructed sfnt bytes.
  std::shared_ptr<const std::vector<uint8_t>> sharedData;  // Shares raw TTF/OTF bytes.
  fonts::SfntFont sfnt;
  FontDataTrust trust = FontDataTrust::Untrusted;
  FontBudgetReservation reservation;

  std::span<const uint8_t> fontData() const {
    if (sharedData) {
      return {sharedData->data(), sharedData->size()};
    }

    return {ownedData.data(), ownedData.size()};
  }
};

namespace {
/// Process-wide default font provider adopted by newly constructed FontManagers. Borrowed, never
/// owned. Atomic so a render thread constructing a FontManager races safely against the editor's
/// one-time install on the main thread.
std::atomic<const FontFamilyProvider*> g_defaultFontProvider{nullptr};
}  // namespace

void FontManager::SetDefaultFontProvider(const FontFamilyProvider* provider) {
  g_defaultFontProvider.store(provider, std::memory_order_release);
}

const FontFamilyProvider* FontManager::DefaultFontProvider() {
  return g_defaultFontProvider.load(std::memory_order_acquire);
}

FontManager::FontManager(Registry& registry, size_t maximumLoadedFontBytes,
                         size_t maximumLoadedFonts, size_t maximumFontValidationWork)
    : registry_(registry),
      provider_(g_defaultFontProvider.load(std::memory_order_acquire)),
      candidateBudgetState_(std::make_shared<FontBudgetState>(FontBudgetState{
          maximumLoadedFontBytes, maximumLoadedFonts, maximumFontValidationWork, 0, 0, 0, {}})) {}
FontManager::~FontManager() = default;

size_t FontManager::ProviderFontKeyHash::operator()(const ProviderFontKey& key) const noexcept {
  // Mixed field by field rather than through a concatenated string, so no field's value can run
  // into the next one's and make two different requests hash and compare as one.
  size_t hash = std::hash<std::string>{}(key.family);
  for (const int field : {key.weight, key.style, key.stretch}) {
    hash ^= std::hash<int>{}(field) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  }
  return hash;
}

void FontManager::addFontFace(const css::FontFace& face) {
  std::string key = css::FontFaceIdentityKey(face);
  if (auto it = faceEntities_.find(key); it != faceEntities_.end()) {
    // The registered entity is also what keeps the declaration's payload pointers alive, so the
    // key stays meaningful exactly as long as that entity still carries the rule.
    if (registry_.valid(it->second) && registry_.all_of<FontFaceComponent>(it->second)) {
      // The identical declaration is already registered. Minting a second entity for it would
      // strand every cache keyed on the old one, so keep the existing identity and leave the
      // resolution cache alone.
      return;
    }
    faceEntities_.erase(it);
  }

  const Entity entity = registry_.create();
  registry_.emplace<FontFaceComponent>(entity, face, nextFaceSequence_++);
  faceEntities_.emplace(std::move(key), entity);
  // A genuinely new declaration can outrank an earlier resolution, so previously resolved queries
  // have to be recomputed.
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
  genericFamilyMap_[ToLowerAscii(genericName)] = std::string(realFamily);
}

FontHandle FontManager::findFont(std::string_view family) {
  return findFont(family, 400);
}

FontHandle FontManager::findFont(std::string_view family, int weight) {
  return findFont(family, weight, 0, 5);
}

FontHandle FontManager::findFont(std::string_view family, int weight, int style, int stretch) {
  // Resolve CSS generic family names to real family names.
  const std::string familyLower = ToLowerAscii(family);
  if (auto it = genericFamilyMap_.find(familyLower); it != genericFamilyMap_.end()) {
    family = it->second;
  }

  const std::string cacheKey = std::string(family) + ":" + std::to_string(weight) + ":" +
                               std::to_string(style) + ":" + std::to_string(stretch);
  if (auto it = cache_.find(cacheKey); it != cache_.end()) {
    // A cached answer outlives many lookups, so confirm the entity behind it is still a live,
    // loaded font rather than handing back a handle to a destroyed or emptied one. Every answer
    // this function caches has loaded bytes, so anything else means the entity changed underneath
    // the cache and the query has to be resolved again.
    if (isValidHandle(it->second) && registry_.all_of<LoadedFontComponent>(it->second.entity())) {
      return it->second;
    }
    cache_.erase(it);
  }

  // Walk @font-face rules looking for the best match.
  // CSS font matching: style first, then stretch, then weight.
  Entity bestEntity = entt::null;
  int bestScore = std::numeric_limits<int>::max();
  uint64_t bestSequence = 0;

  auto view = registry_.view<FontFaceComponent>();
  for (const Entity entity : view) {
    const FontFaceComponent& component = view.get<FontFaceComponent>(entity);
    const css::FontFace& face = component.face;
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
      // Request is narrow, face is even narrower - preferred direction.
      score += (-stretchDelta) * 100;
    } else if (stretch > 5 && stretchDelta > 0) {
      // Request is wide, face is even wider - preferred direction.
      score += stretchDelta * 100;
    } else {
      // Face is on the wrong side - heavy penalty.
      score += std::abs(stretchDelta) * 100 + 1000;
    }

    score += std::abs(face.fontWeight - weight);

    // Equally good faces resolve to the one declared last, per CSS. The scan runs to completion
    // rather than stopping at the first exact match, because a later declaration can tie it and
    // then has to win.
    if (score < bestScore || (score == bestScore && component.sequence > bestSequence)) {
      bestScore = score;
      bestSequence = component.sequence;
      bestEntity = entity;
    }
  }

  // Set when something claimed this family but could not be turned into loaded bytes this time,
  // whether that was an `@font-face` rule or the external provider. The failure can be transient
  // (the aggregate budget was full), and the answer returned below is only a stand-in for it, so
  // that answer must not be cached or the family would never recover.
  bool matchedSourceFailedToLoad = false;

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
        const FontDataTrust trust =
            source.trusted ? FontDataTrust::Trusted : FontDataTrust::Untrusted;
        if (loadFontDataSharedIntoEntity(bestEntity, dataPtr, trust)) {
          FontHandle handle(bestEntity);
          cache_[cacheKey] = handle;
          return handle;
        }
      }
    }
    matchedSourceFailedToLoad = true;
  }

  const auto cacheUnlessRetryable = [&](FontHandle handle) {
    if (!matchedSourceFailedToLoad) {
      cache_[cacheKey] = handle;
    }
    return handle;
  };

  // No document @font-face matched. Consult the external provider (embedded/system catalog) before
  // falling back to Public Sans. The provider itself orders Embedded before System.
  if (provider_ != nullptr && provider_->hasFamily(family)) {
    const FontFaceRequest request{.weight = weight,
                                  .style = static_cast<FontStyle>(style),
                                  .stretch = static_cast<FontStretch>(stretch)};
    const ProviderFontKey providerKey{ToLowerAscii(family), weight, style, stretch};

    if (auto it = providerFonts_.find(providerKey); it != providerFonts_.end()) {
      if (isValidHandle(it->second) && registry_.all_of<LoadedFontComponent>(it->second.entity())) {
        // A provider answers per requested face, so this memo is keyed on the whole request. The
        // same request resolving again reuses the entity already holding those bytes instead of
        // loading a duplicate copy onto a new identity.
        return cacheUnlessRetryable(it->second);
      }
      providerFonts_.erase(it);
    }

    std::vector<uint8_t> data = provider_->loadFamilyData(family, request);
    if (!data.empty()) {
      const Entity entity = registry_.create();
      if (loadFontDataIntoEntity(entity, data, FontDataTrust::Trusted)) {
        FontHandle handle(entity);
        providerFonts_[providerKey] = handle;
        return cacheUnlessRetryable(handle);
      }
      registry_.destroy(entity);
    }

    // The provider claims this family but nothing loadable came back for the requested face, which
    // a full aggregate budget alone is enough to cause. Same reasoning as a document face that
    // failed to load: the answer below stands in for this request until the load can succeed, so
    // it must not stick. Only this request is affected; the family's other faces resolve on their
    // own and are not held back by it.
    matchedSourceFailedToLoad = true;
  }

  // Fall back to the embedded Public Sans font.
  return cacheUnlessRetryable(fallbackFont());
}

FontHandle FontManager::loadFontData(std::span<const uint8_t> data, FontDataTrust trust) {
  const Entity entity = registry_.create();
  if (!loadFontDataIntoEntity(entity, data, trust)) {
    registry_.destroy(entity);
    return FontHandle();
  }

  return FontHandle(entity);
}

bool FontManager::loadFontDataSharedIntoEntity(
    Entity entity, const std::shared_ptr<const std::vector<uint8_t>>& data, FontDataTrust trust) {
  if (!data) {
    return false;
  }

  if (data->size() < 4) {
    return false;
  }

  const uint32_t magic = readBE32(data->data());
  if (!IsSupportedFontMagic(magic)) {
    return false;
  }

  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  if (exhaustedValidationBudgetRejects(*data, trust, budgetState) ||
      isValidationRejectedSource(data, budgetState)) {
    return false;
  }
  bool workLimitExceeded = false;
  const auto finishLoad = [&](bool loaded) {
    rememberValidationRejectedSource(data, workLimitExceeded, budgetState);
    return loaded;
  };

  // WOFF fonts need decompression/reconstruction, so they create new owned buffers.
  if (magic == kWoffMagic) {
    return finishLoad(loadWoff1(entity, *data, trust, &workLimitExceeded));
  }

  if (magic == kWoff2Magic) {
#ifdef DONNER_TEXT_WOFF2_ENABLED
    return finishLoad(loadWoff2(entity, *data, trust, &workLimitExceeded));
#else
    std::cerr << "FontManager: WOFF2 font encountered but WOFF2 support not enabled. "
                 "Build with --config=text-full to enable.\n";
    return false;
#endif
  }

  // Raw TTF/OTF: share the data via shared_ptr (no copy).
  return finishLoad(setRawFontData(entity, data, trust, &workLimitExceeded));
}

bool FontManager::isValidationRejectedSource(
    const std::shared_ptr<const std::vector<uint8_t>>& data,
    const std::shared_ptr<FontBudgetState>& budgetState) const {
  const auto found = budgetState->validationRejectedSources.find(data.get());
  if (found == budgetState->validationRejectedSources.end()) return false;
  if (found->second.lock() == data) return true;
  budgetState->validationRejectedSources.erase(found);
  return false;
}

bool FontManager::exhaustedValidationBudgetRejects(
    std::span<const uint8_t> data, FontDataTrust trust,
    const std::shared_ptr<FontBudgetState>& budgetState) const {
  return trust == FontDataTrust::Untrusted &&
         budgetState->usedValidationWork >= budgetState->maximumValidationWork &&
         HasCffFlavor(data);
}

void FontManager::rememberValidationRejectedSource(
    const std::shared_ptr<const std::vector<uint8_t>>& data, bool workLimitExceeded,
    const std::shared_ptr<FontBudgetState>& budgetState) {
  if (workLimitExceeded &&
      budgetState->validationRejectedSources.size() < budgetState->maximumFonts) {
    budgetState->validationRejectedSources.insert_or_assign(data.get(), data);
  }
}

std::span<const uint8_t> FontManager::fontData(FontHandle handle) const {
  if (!isValidHandle(handle)) {
    return {};
  }

  const auto* font = registry_.try_get<LoadedFontComponent>(handle.entity());
  return font ? font->fontData() : std::span<const uint8_t>();
}

bool FontManager::isTrustedFont(FontHandle handle) const {
  if (!isValidHandle(handle)) {
    return false;
  }

  const auto* font = registry_.try_get<LoadedFontComponent>(handle.entity());
  return font != nullptr && font->trust == FontDataTrust::Trusted;
}

std::optional<FontManager::GlyphOutlineComplexity> FontManager::glyphOutlineComplexity(
    FontHandle handle, int glyphIndex) const {
  if (!isValidHandle(handle) || glyphIndex < 0) {
    return std::nullopt;
  }
  const auto* font = registry_.try_get<LoadedFontComponent>(handle.entity());
  if (!font) {
    return std::nullopt;
  }
  const auto complexity = font->sfnt.glyphOutlineComplexity(static_cast<std::size_t>(glyphIndex));
  if (!complexity) {
    return std::nullopt;
  }
  return GlyphOutlineComplexity{
      .maximumVertices = complexity->maximumVertices,
      .work = complexity->work,
  };
}

std::optional<std::span<const uint8_t>> FontManager::sfntTable(FontHandle handle,
                                                               std::string_view tag) const {
  if (!isValidHandle(handle)) {
    return std::nullopt;
  }
  const auto* font = registry_.try_get<LoadedFontComponent>(handle.entity());
  return font ? font->sfnt.findTable(font->fontData(), tag) : std::nullopt;
}

bool FontManager::isValidatedFont(FontHandle handle) const {
  return isValidHandle(handle) && registry_.all_of<LoadedFontComponent>(handle.entity());
}

size_t FontManager::loadedFontBytes() const {
  return budgetStateForRead()->usedBytes;
}

size_t FontManager::numLoadedFonts() const {
  return budgetStateForRead()->usedFonts;
}

size_t FontManager::fontValidationWork() const {
  return budgetStateForRead()->usedValidationWork;
}

size_t FontManager::numValidationRejectedSources() const {
  return budgetStateForRead()->validationRejectedSources.size();
}

FontHandle FontManager::fallbackFont() {
  if (isValidHandle(fallbackHandle_)) {
    return fallbackHandle_;
  }

  const Entity entity = registry_.create();

  // Load the embedded Public Sans font.
  std::vector<uint8_t> data(embedded::kPublicSansMediumOtf.begin(),
                            embedded::kPublicSansMediumOtf.end());
  if (!setRawFontData(entity, std::move(data), FontDataTrust::Trusted)) {
    registry_.destroy(entity);
    std::cerr << "FontManager: Failed to load embedded fallback font (Public Sans)\n";
    return FontHandle();
  }

  fallbackHandle_ = FontHandle(entity);
  return fallbackHandle_;
}

std::optional<fonts::SfntFont> FontManager::validateSfntForLoad(
    Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
    const std::shared_ptr<FontBudgetState>& budgetState, bool* validationWorkLimitExceeded) {
  if (validationWorkLimitExceeded) {
    *validationWorkLimitExceeded = false;
  }
  if (!canStoreLoadedFont(entity, data.size(), 0, budgetState)) {
    return std::nullopt;
  }
  if (exhaustedValidationBudgetRejects(data, trust, budgetState)) {
    if (validationWorkLimitExceeded) {
      *validationWorkLimitExceeded = true;
    }
    return std::nullopt;
  }

  assert(budgetState->usedValidationWork <= budgetState->maximumValidationWork);
  const size_t remainingValidationWork =
      budgetState->maximumValidationWork - budgetState->usedValidationWork;
  const size_t perCallValidationWork =
      trust == FontDataTrust::Trusted
          ? fonts::kMaximumCffOutlineValidationWork
          : std::min(remainingValidationWork, fonts::kMaximumCffOutlineValidationWork);
  fonts::SfntValidationMetrics metrics;
  auto sfnt = fonts::SfntFont::Validate(data, {.maximumCffValidationWork = perCallValidationWork},
                                        &metrics);
  if (trust == FontDataTrust::Untrusted) {
    assert(metrics.cffValidationWork <= remainingValidationWork);
    budgetState->usedValidationWork += metrics.cffValidationWork;
  }
  if (metrics.cffWorkLimitExceeded) {
    if (validationWorkLimitExceeded) {
      *validationWorkLimitExceeded = true;
    }
    return std::nullopt;
  }
  return sfnt;
}

bool FontManager::setRawFontData(Entity entity, std::vector<uint8_t> data, FontDataTrust trust,
                                 bool* validationWorkLimitExceeded) {
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  auto sfnt = validateSfntForLoad(entity, data, trust, budgetState, validationWorkLimitExceeded);
  if (!sfnt) {
    return false;
  }

  LoadedFontComponent font;
  font.ownedData = std::move(data);
  font.sfnt = std::move(*sfnt);
  font.trust = trust;
  return storeLoadedFont(entity, std::move(font));
}

bool FontManager::setRawFontData(Entity entity,
                                 std::shared_ptr<const std::vector<uint8_t>> sharedData,
                                 FontDataTrust trust, bool* validationWorkLimitExceeded) {
  if (!sharedData) {
    return false;
  }
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  auto sfnt =
      validateSfntForLoad(entity, *sharedData, trust, budgetState, validationWorkLimitExceeded);
  if (!sfnt) {
    return false;
  }

  LoadedFontComponent font;
  font.sharedData = std::move(sharedData);
  font.sfnt = std::move(*sfnt);
  font.trust = trust;
  return storeLoadedFont(entity, std::move(font));
}

std::shared_ptr<const FontManager::FontBudgetState> FontManager::budgetStateForRead() const {
  const Registry& registry = registry_;
  if (const auto* context = registry.ctx().find<FontBudgetContext>()) {
    assert(context->state);
    return context->state;
  }
  return candidateBudgetState_;
}

std::shared_ptr<FontManager::FontBudgetState> FontManager::budgetStateForWrite() {
  if (const auto* context = registry_.ctx().find<FontBudgetContext>()) {
    assert(context->state);
    return context->state;
  }

  registry_.ctx().emplace<FontBudgetContext>(FontBudgetContext{candidateBudgetState_});
  return candidateBudgetState_;
}

bool FontManager::storeLoadedFont(Entity entity, LoadedFontComponent font) {
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  const size_t rawBytes = font.fontData().size();
  const size_t indexBytes = font.sfnt.retainedBytes();
  if (!canStoreLoadedFont(entity, rawBytes, indexBytes, budgetState)) {
    return false;
  }
  const size_t chargeBytes = rawBytes + indexBytes;

  if (registry_.all_of<LoadedFontComponent>(entity)) {
    registry_.remove<LoadedFontComponent>(entity);
  }
  font.reservation = FontBudgetReservation(budgetState, chargeBytes);
  registry_.emplace<LoadedFontComponent>(entity, std::move(font));
  return true;
}

bool FontManager::canStoreLoadedFont(Entity entity, size_t rawBytes, size_t indexBytes,
                                     const std::shared_ptr<FontBudgetState>& budgetState) const {
  if (rawBytes > budgetState->maximumBytes || indexBytes > budgetState->maximumBytes - rawBytes) {
    return false;
  }
  const size_t chargeBytes = rawBytes + indexBytes;

  const auto* previous = registry_.try_get<LoadedFontComponent>(entity);
  if (previous && previous->reservation.state != budgetState) {
    return false;
  }
  const size_t previousBytes = previous ? previous->reservation.bytes : 0;
  const size_t previousFonts = previous ? 1 : 0;
  assert(budgetState->usedBytes >= previousBytes);
  assert(budgetState->usedFonts >= previousFonts);
  const size_t retainedBytes = budgetState->usedBytes - previousBytes;
  const size_t retainedFonts = budgetState->usedFonts - previousFonts;
  if (chargeBytes > budgetState->maximumBytes - retainedBytes ||
      retainedFonts >= budgetState->maximumFonts) {
    return false;
  }
  return true;
}

bool FontManager::loadWoff1(Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
                            bool* validationWorkLimitExceeded) {
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  fonts::WoffParser::Options options;
  options.maximumSfntSize = std::min(options.maximumSfntSize, budgetState->maximumBytes);
  auto maybeFont = fonts::WoffParser::Parse(data, options);
  if (maybeFont.hasError()) {
    std::cerr << "FontManager: WOFF1 parsing failed: " << maybeFont.error().reason << "\n";
    return false;
  }

  // Reconstruct sfnt byte stream from decompressed WOFF tables.
  std::vector<uint8_t> sfntData = reconstructSfnt(maybeFont.result());
  return setRawFontData(entity, std::move(sfntData), trust, validationWorkLimitExceeded);
}

bool FontManager::loadFontDataIntoEntity(Entity entity, std::span<const uint8_t> data,
                                         FontDataTrust trust) {
  if (data.size() < 4) {
    return false;
  }

  const uint32_t magic = readBE32(data.data());

  if (!IsSupportedFontMagic(magic)) {
    return false;
  }

  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  if (exhaustedValidationBudgetRejects(data, trust, budgetState)) return false;

  if (magic == kWoffMagic) {
    return loadWoff1(entity, data, trust);
  }

  if (magic == kWoff2Magic) {
#ifdef DONNER_TEXT_WOFF2_ENABLED
    return loadWoff2(entity, data, trust);
#else
    std::cerr << "FontManager: WOFF2 font encountered but WOFF2 support not enabled. "
                 "Build with --config=text-full to enable.\n";
    return false;
#endif
  }

  // Validate and budget the retained index before copying the untrusted byte stream.
  auto sfnt = validateSfntForLoad(entity, data, trust, budgetState);
  if (!sfnt) {
    return false;
  }
  if (!canStoreLoadedFont(entity, data.size(), sfnt->retainedBytes(), budgetState)) {
    return false;
  }
  LoadedFontComponent font;
  font.ownedData.assign(data.begin(), data.end());
  font.sfnt = std::move(*sfnt);
  font.trust = trust;
  return storeLoadedFont(entity, std::move(font));
}

#ifdef DONNER_TEXT_WOFF2_ENABLED
bool FontManager::loadWoff2(Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
                            bool* validationWorkLimitExceeded) {
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  fonts::Woff2Parser::Options options;
  options.maximumOutputSize = std::min(options.maximumOutputSize, budgetState->maximumBytes);
  auto result = fonts::Woff2Parser::Decompress(data, options);
  if (result.hasError()) {
    std::cerr << "FontManager: WOFF2 decompression failed: " << result.error().reason << "\n";
    return false;
  }

  return setRawFontData(entity, std::move(result.result()), trust, validationWorkLimitExceeded);
}
#endif

bool FontManager::isValidHandle(FontHandle handle) const {
  return handle && registry_.valid(handle.entity());
}

}  // namespace donner::svg
