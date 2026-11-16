#include "donner/svg/resources/FontManager.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/StringUtils.h"
#include "donner/base/fonts/SfntUtils.h"
#include "donner/base/fonts/WoffFont.h"
#include "donner/base/fonts/WoffParser.h"
#ifdef DONNER_TEXT_WOFF2_ENABLED
#include "donner/base/fonts/Woff2Parser.h"
#endif
#include "donner/svg/resources/FontMetadata.h"
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

using FontMatchScore = std::tuple<int, double, int, int, int64_t>;

std::pair<int, double> FontWidthRank(const css::FontFace& face, int requestedWidth) {
  const double requested = css::FontFace::FontStretchPercentage(requestedWidth);
  const auto [minimum, maximum] = face.normalizedFontStretchRange();
  if (minimum <= requested && requested <= maximum) {
    return {0, 0};
  }
  const bool narrower = maximum < requested;
  const bool preferred = requested <= 100 ? narrower : !narrower;
  // Subtracting tiny endpoints from a requested width can erase their ordering.
  return {preferred ? 1 : 2, narrower ? -maximum : minimum};
}

int FontStyleRank(int actual, int requested) {
  if (actual == requested) {
    return 0;
  }
  if (requested == 0) {
    return actual == 2 ? 1 : 2;
  }
  return (actual == 1 && requested == 2) || (actual == 2 && requested == 1) ? 1 : 2;
}

std::pair<int, int64_t> FontWeightRank(const css::FontFace& face, int requested) {
  const auto [minimum, maximum] = face.normalizedFontWeightRange();
  if (minimum <= requested && requested <= maximum) {
    return {0, 0};
  }
  if (requested < 400) {
    return maximum < requested ? std::pair<int, int64_t>{1, requested - maximum}
                               : std::pair<int, int64_t>{2, minimum - requested};
  }
  if (requested > 500) {
    return minimum > requested ? std::pair<int, int64_t>{1, minimum - requested}
                               : std::pair<int, int64_t>{2, requested - maximum};
  }
  if (minimum > requested && minimum <= 500) {
    return {1, minimum - requested};
  }
  return maximum < requested ? std::pair<int, int64_t>{2, requested - maximum}
                             : std::pair<int, int64_t>{3, minimum - requested};
}

FontMatchScore RankFontFace(const css::FontFace& face, int weight, int style, int stretch) {
  const auto [widthDirection, widthOrder] = FontWidthRank(face, stretch);
  const auto [weightDirection, weightDistance] = FontWeightRank(face, weight);
  // Filter by width before style, then by weight. Numeric weight distance cannot outweigh
  // a better width/style match, even when a declaration covers a wide weight interval.
  return {widthDirection, widthOrder, FontStyleRank(face.fontStyle, style), weightDirection,
          weightDistance};
}

/// Preserve only lossless names: the metadata decoder substitutes '?' for non-ASCII code points.
std::string ActualFontFamily(std::span<const uint8_t> data) {
  auto metadata = ParseFontMetadata(data);
  if (!metadata || std::any_of(metadata->familyName.begin(), metadata->familyName.end(),
                               [](char ch) { return ch < ' ' || ch >= 127 || ch == '?'; })) {
    return {};
  }
  return std::move(metadata->familyName);
}

/// Check the complete charge without overflowing the byte-budget sum.
bool FitsFontByteBudget(size_t rawBytes, size_t cachedBytes, size_t maximumBytes) {
  return rawBytes <= maximumBytes && cachedBytes <= maximumBytes - rawBytes;
}

/// Read a uint32_t in big-endian from a byte pointer.
uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// WOFF flavor fields cannot prove that decompressed tables omit CFF, so compressed inputs share
/// the exhausted-budget path regardless of their advertised flavor.
bool IsCompressedOrCffFont(std::span<const uint8_t> data) {
  if (data.size() < 4) {
    return false;
  }
  const uint32_t magic = readBE32(data.data());
  return magic == kSfntCff || magic == kWoffMagic || magic == kWoff2Magic;
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
    const size_t paddedLen = (table.data.size() + 3) & ~size_t{3};
    writeBE32(dir, table.tag);
    writeBE32(dir + 4, fonts::SfntTableChecksum(table.data));
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

/// A face lent by a host document's \ref FontManager::HostFontOverlay. A distinct component, so
/// the manager's own rules, counts and family index never include it.
struct FontManager::HostFontFaceComponent : FontManager::FontFaceComponent {
  using FontFaceComponent::FontFaceComponent;
};

struct FontManager::FontBudgetState {
  size_t maximumBytes = 0;
  size_t maximumFonts = 0;
  size_t maximumValidationWork = 0;
  size_t usedBytes = 0;
  size_t usedFonts = 0;
  size_t usedValidationWork = 0;
  size_t compressedFontDecompressionAttempts = 0;
  std::unordered_map<const std::vector<uint8_t>*, std::weak_ptr<const std::vector<uint8_t>>>
      validationRejectedSources;
  uint64_t retainedRevision = 0;
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
    ++state->retainedRevision;
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
    ++state->retainedRevision;
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
  std::string actualFamily;  // Owning storage with capacity charged to the font budget.
  FontDataTrust trust = FontDataTrust::Untrusted;
  std::optional<FontFaceRequest> providerRequest;
  bool genericSans = false;
  bool immutableCatalog = false;
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
          maximumLoadedFontBytes, maximumLoadedFonts, maximumFontValidationWork, 0, 0, 0, {}})) {
  // Another manager may have registered rules on this registry already, and hasFamily() must not
  // write, so the family index is complete before the first query rather than filled by one.
  auto view = registry_.view<FontFaceComponent>();
  for (const Entity entity : view) {
    registeredFamiliesLower_.insert(
        StringUtils::ToLowerAscii(view.get<FontFaceComponent>(entity).face.familyName));
  }
}
FontManager::~FontManager() = default;

size_t FontManager::ProviderFontKeyHash::operator()(const ProviderFontKey& key) const noexcept {
  // Mixed field by field rather than through a concatenated string, so no field's value can run
  // into the next one's and make two different requests hash and compare as one.
  size_t hash = std::hash<std::string>{}(key.family);
  for (const int field : {key.weight, key.style, key.stretch}) {
    hash ^= std::hash<int>{}(field) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  }
  hash ^=
      std::hash<std::string>{}(key.contentId) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  hash ^= std::hash<uint64_t>{}(key.contentGeneration) + 0x9e3779b97f4a7c15ULL + (hash << 6) +
          (hash >> 2);
  return hash;
}

FontManager::DependencyCapture::DependencyCapture(FontManager& manager,
                                                  std::vector<FontFaceDependency>& output)
    : manager_(manager), previous_(manager.dependencyCapture_) {
  output.clear();
  manager_.dependencyCapture_ = &output;
}

FontManager::DependencyCapture::~DependencyCapture() {
  auto* completed = manager_.dependencyCapture_;
  manager_.dependencyCapture_ = previous_;
  if (previous_) {
    for (const auto& dependency : *completed) {
      const ProviderFontKey key{dependency.family, dependency.request.weight,
                                static_cast<int>(dependency.request.style),
                                static_cast<int>(dependency.request.stretch)};
      manager_.recordDependency(key);
    }
  }
}

void FontManager::recordDependency(const ProviderFontKey& key) {
  if (!dependencyCapture_) {
    return;
  }
  const auto it = providerDependencies_.find(key);
  if (it == providerDependencies_.end()) {
    return;
  }
  const auto existing = std::find_if(dependencyCapture_->begin(), dependencyCapture_->end(),
                                     [&](const FontFaceDependency& dependency) {
                                       return dependency.family == it->second.family &&
                                              dependency.request == it->second.request;
                                     });
  if (existing != dependencyCapture_->end()) {
    *existing = it->second;
  } else if (dependencyCapture_->size() < budgetStateForRead()->maximumFonts) {
    dependencyCapture_->push_back(it->second);
  } else {
    fontDependenciesOverflowed_ = true;
  }
}

void FontManager::recordDependencies(std::span<const FontFaceDependency> dependencies) {
  for (const auto& dependency : dependencies) {
    recordDependency({dependency.family, dependency.request.weight,
                      static_cast<int>(dependency.request.style),
                      static_cast<int>(dependency.request.stretch)});
  }
}

void FontManager::rememberDependency(const ProviderFontKey& key, const FontFaceRequest& request,
                                     const FontFaceAvailability& availability,
                                     FontFaceLoadState state, FontFaceWaitReason waitReason) {
  if (availability.contentId.empty()) {
    return;
  }
  if (!providerDependencies_.contains(key) &&
      providerDependencies_.size() >= budgetStateForRead()->maximumFonts) {
    fontDependenciesOverflowed_ = true;
    return;
  }
  if (const auto previous = providerDependencies_.find(key);
      previous != providerDependencies_.end() &&
      (previous->second.availability.contentId != availability.contentId ||
       previous->second.availability.contentGeneration != availability.contentGeneration)) {
    ProviderFontKey obsolete = key;
    obsolete.contentId = previous->second.availability.contentId;
    obsolete.contentGeneration = previous->second.availability.contentGeneration;
    providerFailures_.erase(obsolete);
  }
  providerDependencies_[key] = {key.family,
                                request,
                                availability,
                                state,
                                waitReason,
                                waitReason == FontFaceWaitReason::RetainedBudget
                                    ? budgetStateForRead()->retainedRevision
                                    : 0};
  recordDependency(key);
}

std::vector<FontFaceDependency> FontManager::faceDependencies() const {
  std::vector<FontFaceDependency> result;
  result.reserve(providerDependencies_.size());
  for (const auto& [key, dependency] : providerDependencies_) {
    result.push_back(dependency);
  }
  return result;
}

bool FontManager::refreshPendingFonts() {
  const uint64_t revision = fontResourceRevision_;
  const bool previousRefreshing = refreshingFonts_;
  refreshingFonts_ = true;
  // A retry updates the map; do not retain map iterators across resolution.
  const auto dependencies = faceDependencies();
  for (const auto& dependency : dependencies) {
    const auto current = provider_ ? provider_->availability(dependency.family, dependency.request)
                                   : FontFaceAvailability{};
    if (dependency.waitReason == FontFaceWaitReason::RetainedBudget &&
        dependency.consumerBudgetRevision == budgetStateForRead()->retainedRevision &&
        current == dependency.availability) {
      continue;
    }
    if (dependency.state != FontFaceLoadState::Loaded ||
        current.contentId != dependency.availability.contentId ||
        current.contentGeneration != dependency.availability.contentGeneration) {
      findFont(dependency.family, dependency.request.weight,
               static_cast<int>(dependency.request.style),
               static_cast<int>(dependency.request.stretch));
    }
  }
  refreshingFonts_ = previousRefreshing;
  return revision != fontResourceRevision_;
}

bool FontManager::hasUnresolvedDependencies() const {
  return fontDependenciesOverflowed_ ||
         std::any_of(
             providerDependencies_.begin(), providerDependencies_.end(),
             [](const auto& entry) { return entry.second.state != FontFaceLoadState::Loaded; });
}

bool FontManager::admissionCanProgress(const FontFaceDependency& dependency,
                                       const FontFaceAvailability& current) const {
  return current != dependency.availability ||
         dependency.waitReason != FontFaceWaitReason::RetainedBudget ||
         dependency.consumerBudgetRevision != budgetStateForRead()->retainedRevision;
}

bool FontManager::dependencyNeedsResourceRefresh(const ProviderFontKey& key,
                                                 const FontFaceDependency& dependency) const {
  const auto current = provider_->availability(dependency.family, dependency.request);
  if (current.contentId != dependency.availability.contentId ||
      current.contentGeneration != dependency.availability.contentGeneration) {
    return true;
  }
  if (dependency.state == FontFaceLoadState::WaitingForAdmission) {
    // A ready global asset does not make an unchanged retained budget eligible.
    return admissionCanProgress(dependency, current);
  }
  if (current.state == FontAssetState::Failed && dependency.state != FontFaceLoadState::Failed) {
    return true;
  }
  if (current.state == FontAssetState::Ready && dependency.state != FontFaceLoadState::Loaded) {
    auto contentKey = key;
    contentKey.contentId = current.contentId;
    contentKey.contentGeneration = current.contentGeneration;
    return !providerFailures_.contains(contentKey);
  }
  return false;
}

bool FontManager::needsResourceRefresh() const {
  if (!provider_) {
    return false;
  }
  for (const auto& [key, dependency] : providerDependencies_) {
    if (dependencyNeedsResourceRefresh(key, dependency)) {
      return true;
    }
  }
  return false;
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
  registeredFamiliesLower_.insert(StringUtils::ToLowerAscii(face.familyName));
  // A genuinely new declaration can outrank an earlier resolution, so previously resolved queries
  // have to be recomputed.
  cache_.clear();
  clearLentSetCaches();
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
  std::string& mapped = genericFamilyMap_[StringUtils::ToLowerAscii(genericName)];
  if (mapped != realFamily) {
    mapped = std::string(realFamily);
    familySelectionRevision_ = ++fontResourceRevision_;
  }
}

void FontManager::setDefaultFamily(std::string_view family) {
  if (defaultFamily_ != family) {
    defaultFamily_ = std::string(family);
    familySelectionRevision_ = ++fontResourceRevision_;
  }
}

bool FontManager::hasDefaultFamily() const {
  return !defaultFamily_.empty() && hasFamily(defaultFamily_);
}

FontHandle FontManager::defaultFont(int weight, int style, int stretch) {
  if (hasDefaultFamily()) {
    if (const FontHandle font = findFont(defaultFamily_, weight, style, stretch)) {
      return font;
    }
  }
  return fallbackFont();
}

namespace {

/// Mix @p value into @p hash.
uint64_t MixHash(uint64_t hash, uint64_t value) {
  return hash ^ (value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2));
}

/// @p face reduced to the sources a host can lend, or nullopt when none of them is already data.
/// A face with nothing to lend is not copied; a lent copy shares the payloads by reference.
std::optional<css::FontFace> LendableFace(const css::FontFace& face) {
  const auto isData = [](const css::FontFaceSource& source) {
    return source.kind == css::FontFaceSource::Kind::Data;
  };
  if (std::none_of(face.sources.begin(), face.sources.end(), isData)) {
    return std::nullopt;
  }
  css::FontFace lent = face;
  std::erase_if(lent.sources, [&](const css::FontFaceSource& source) { return !isData(source); });
  return lent;
}

/// The generic mappings a host lends: its own, then those it was lent for names it does not map,
/// sorted by generic name.
std::vector<std::pair<std::string, std::string>> LentGenericFamilies(
    const std::unordered_map<std::string, std::string>& own,
    const FontManager::HostFontOverlay* inherited) {
  std::vector<std::pair<std::string, std::string>> result(own.begin(), own.end());
  if (inherited) {
    std::copy_if(inherited->genericFamilies.begin(), inherited->genericFamilies.end(),
                 std::back_inserter(result),
                 [&](const auto& mapping) { return !own.contains(mapping.first); });
  }
  std::sort(result.begin(), result.end());
  return result;
}

}  // namespace

std::shared_ptr<const FontManager::HostFontOverlay> FontManager::HostFontOverlay::Make(
    std::span<const css::FontFace> faces, const FontManager* hostFonts) {
  auto overlay = std::make_shared<HostFontOverlay>();
  uint64_t digest = 0;
  // A host that is itself referenced passes on what its own host lent it. Those faces come first,
  // so on a tie the nearer host's face, declared later, wins.
  const auto inherited = hostFonts ? hostFonts->hostFontOverlay() : nullptr;
  if (inherited) {
    overlay->faces = inherited->faces;
    overlay->faceKeys = inherited->faceKeys;
    digest = inherited->digest;
  }
  for (const css::FontFace& face : faces) {
    if (overlay->faces.size() >= kMaximumLentFaces) {
      break;
    }
    if (auto lent = LendableFace(face)) {
      std::string key = css::FontFaceIdentityKey(*lent);
      digest = MixHash(digest, std::hash<std::string>{}(key));
      overlay->faceKeys.push_back(std::move(key));
      overlay->faces.push_back(std::move(*lent));
    }
  }
  if (hostFonts) {
    // The nearer host's mapping for a generic name wins over the one it was lent.
    overlay->genericFamilies =
        LentGenericFamilies(hostFonts->genericFamilyMappings(), inherited.get());
  }
  for (const auto& [generic, realFamily] : overlay->genericFamilies) {
    digest = MixHash(digest, std::hash<std::string>{}(generic));
    digest = MixHash(digest, std::hash<std::string>{}(realFamily));
  }
  if (overlay->faces.empty() && overlay->genericFamilies.empty()) {
    return nullptr;
  }
  // Zero means "nothing lent" to \ref hostFontKey.
  overlay->digest = digest == 0 ? 1 : digest;
  return overlay;
}

const FontManager::LentSet* FontManager::activeLentSet() const {
  if (!hostOverlayActive_) {
    return nullptr;
  }
  const auto it = lentSets_.find(activeDigest_);
  return it == lentSets_.end() ? nullptr : &it->second;
}

FontManager::LentSet* FontManager::activeLentSet() {
  return const_cast<LentSet*>(std::as_const(*this).activeLentSet());
}

std::shared_ptr<const FontManager::HostFontOverlay> FontManager::hostFontOverlay() const {
  const LentSet* set = activeLentSet();
  return set ? set->overlay : nullptr;
}

uint64_t FontManager::hostFontKey() const {
  const LentSet* set = activeLentSet();
  return set ? set->key : 0;
}

void FontManager::setHostFontOverlay(std::shared_ptr<const HostFontOverlay> overlay) {
  if (!overlay) {
    hostOverlayActive_ = false;
    return;
  }
  hostOverlayActive_ = true;
  activeDigest_ = overlay->digest;
  if (const auto existing = lentSets_.find(activeDigest_); existing != lentSets_.end()) {
    existing->second.lastLent = ++lentClock_;
    return;
  }
  LentSet& set = lentSets_[activeDigest_];
  set.overlay = std::move(overlay);
  set.lastLent = ++lentClock_;
  uint64_t serials = 0;
  for (size_t i = 0; i < set.overlay->faces.size(); ++i) {
    const css::FontFace& face = set.overlay->faces[i];
    auto [it, inserted] = lentFaces_.try_emplace(set.overlay->faceKeys[i]);
    if (inserted) {
      it->second.entity = registry_.create();
      registry_.emplace<HostFontFaceComponent>(it->second.entity, face, 0);
      it->second.serial = ++lentSerial_;
    }
    ++it->second.sets;
    serials = MixHash(serials, it->second.serial);
    set.faces.emplace_back(it->second.entity, i);
    set.familiesLower.insert(StringUtils::ToLowerAscii(face.familyName));
  }
  lentSetFaces_ += set.faces.size();
  set.key = MixHash(activeDigest_, serials) | 1;
  ++hostOverlayStats_.rebuilds;
  (void)releaseLentSets(kMaximumRetainedLentFaces);
}

std::unordered_map<uint64_t, FontManager::LentSet>::iterator FontManager::leastRecentlyLentSet() {
  auto victim = lentSets_.end();
  for (auto it = lentSets_.begin(); it != lentSets_.end(); ++it) {
    const bool lentNow = hostOverlayActive_ && it->first == activeDigest_;
    if (!lentNow && (victim == lentSets_.end() || it->second.lastLent < victim->second.lastLent)) {
      victim = it;
    }
  }
  return victim;
}

size_t FontManager::releaseLentSet(std::unordered_map<uint64_t, LentSet>::iterator set) {
  size_t released = 0;
  for (const auto& [entity, position] : set->second.faces) {
    const auto face = lentFaces_.find(set->second.overlay->faceKeys[position]);
    if (face != lentFaces_.end() && --face->second.sets == 0) {
      // Destroying the entity releases its loaded bytes and their budget reservation.
      registry_.destroy(face->second.entity);
      lentFaces_.erase(face);
      ++released;
    }
  }
  lentSetFaces_ -= set->second.faces.size();
  lentSets_.erase(set);
  return released;
}

size_t FontManager::releaseLentSets(size_t maximumFaces) {
  size_t released = 0;
  while (lentSetFaces_ > maximumFaces || lentSets_.size() > kMaximumLentSets) {
    const auto victim = leastRecentlyLentSet();
    if (victim == lentSets_.end()) {
      break;
    }
    released += releaseLentSet(victim);
  }
  hostOverlayStats_.evictions += released;
  return released;
}

void FontManager::clearLentSetCaches() {
  for (auto& [digest, set] : lentSets_) {
    set.cache.clear();
  }
}

FontManager::ResolutionCache& FontManager::resolutionCache() {
  LentSet* set = activeLentSet();
  return set ? set->cache : cache_;
}

bool FontManager::loadFaceData(Entity entity, const css::FontFace& face, bool lent) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    bool budgetRefused = false;
    for (const auto& source : face.sources) {
      if (source.kind != css::FontFaceSource::Kind::Data) {
        continue;
      }
      const auto& dataPtr = std::get<std::shared_ptr<const std::vector<uint8_t>>>(source.payload);
      if (loadFontDataSharedIntoEntity(
              entity, dataPtr, source.trusted ? FontDataTrust::Trusted : FontDataTrust::Untrusted,
              &budgetRefused)) {
        return true;
      }
    }
    // Only a budget refusal can be cured by releasing lent bytes, and lent bytes never outrank
    // this document's own faces for its budget.
    if (lent || !budgetRefused || releaseLentSets(0) == 0) {
      return false;
    }
  }
  return false;
}

std::string_view FontManager::resolveGenericFamily(std::string_view family) const {
  const std::string familyLower = StringUtils::ToLowerAscii(family);
  if (const auto generic = genericFamilyMap_.find(familyLower);
      generic != genericFamilyMap_.end()) {
    return generic->second;
  }
  if (const LentSet* set = activeLentSet()) {
    for (const auto& [generic, realFamily] : set->overlay->genericFamilies) {
      if (generic == familyLower) {
        return realFamily;
      }
    }
  }
  return family;
}

FontManager::FaceMatch FontManager::bestFace(std::string_view family, int weight, int style,
                                             int stretch) const {
  // CSS font matching: width first, then style, then weight.
  FaceMatch best;
  std::optional<FontMatchScore> bestScore;
  uint64_t bestSequence = 0;
  const auto consider = [&](Entity entity, const css::FontFace& face, uint64_t sequence,
                            bool lent) {
    if (!StringUtils::Equals<StringComparison::IgnoreCase>(face.familyName, family)) {
      return;
    }
    const FontMatchScore score = RankFontFace(face, weight, style, stretch);

    // Equally good faces resolve to the one declared last, per CSS. The scan runs to completion
    // rather than stopping at the first exact match, because a later declaration can tie it and
    // then has to win.
    if (!bestScore || score < *bestScore || (score == *bestScore && sequence > bestSequence)) {
      bestScore = score;
      bestSequence = sequence;
      best = FaceMatch{entity, lent};
    }
  };

  auto view = registry_.view<FontFaceComponent>();
  for (const Entity entity : view) {
    const FontFaceComponent& component = view.get<FontFaceComponent>(entity);
    consider(entity, component.face, component.sequence, false);
  }
  // The document's own rules win; a lent host face only answers a family they do not declare.
  const LentSet* set = best.entity == entt::null ? activeLentSet() : nullptr;
  if (set != nullptr) {
    for (const auto& [entity, position] : set->faces) {
      consider(entity, registry_.get<HostFontFaceComponent>(entity).face, position, true);
    }
  }
  return best;
}

const css::FontFace& FontManager::matchedFace(const FaceMatch& match) const {
  return match.lent ? registry_.get<HostFontFaceComponent>(match.entity).face
                    : registry_.get<FontFaceComponent>(match.entity).face;
}

bool FontManager::hasFamily(std::string_view family) const {
  const std::string_view resolved = resolveGenericFamily(family);
  const std::string resolvedLower = StringUtils::ToLowerAscii(resolved);

  if (registeredFamiliesLower_.count(resolvedLower) != 0 ||
      providerResolvedFamiliesLower_.count(resolvedLower) != 0 ||
      (activeLentSet() != nullptr && activeLentSet()->familiesLower.count(resolvedLower) != 0)) {
    return true;
  }

  return provider_ != nullptr && provider_->hasFamily(resolved);
}

FontHandle FontManager::findFont(std::string_view family) {
  return findFont(family, 400);
}

FontHandle FontManager::findFont(std::string_view family, int weight) {
  return findFont(family, weight, 0, 5);
}

FontHandle FontManager::findFont(std::string_view family, int weight, int style, int stretch) {
  weight = std::clamp(weight, 1, 1000);
  stretch = std::clamp(stretch, 1, 9);
  // Resolve CSS generic family names to real family names.
  family = resolveGenericFamily(family);

  const ProviderFontKey dependencyKey{StringUtils::ToLowerAscii(family), weight, style, stretch};
  const std::string cacheKey = std::string(family) + ":" + std::to_string(weight) + ":" +
                               std::to_string(style) + ":" + std::to_string(stretch);
  // Answers under an overlay can differ from the document's own, so they are cached apart.
  auto& cache = resolutionCache();
  if (auto it = cache.find(cacheKey); it != cache.end()) {
    // A cached answer outlives many lookups, so confirm the entity behind it is still a live,
    // loaded font rather than handing back a handle to a destroyed or emptied one, and that a
    // catalog's content has not changed since. Every answer this function caches has loaded
    // bytes, so anything else means the answer is stale and the query has to be resolved again.
    const FontHandle font = it->second.font;
    if (isValidHandle(font) && registry_.all_of<LoadedFontComponent>(font.entity()) &&
        providerAnswerIsCurrent(family, dependencyKey, it->second)) {
      recordDependency(dependencyKey);
      return font;
    }
    cache.erase(it);
  }

  // Walk @font-face rules looking for the best match.
  const FaceMatch match = bestFace(family, weight, style, stretch);
  const Entity bestEntity = match.entity;

  // Set when something claimed this family but could not be turned into loaded bytes this time,
  // whether that was an `@font-face` rule or the external provider. The failure can be transient
  // (the aggregate budget was full), and the answer returned below is only a stand-in for it, so
  // that answer must not be cached or the family would never recover.
  bool matchedSourceFailedToLoad = false;

  if (bestEntity != entt::null) {
    // A provider answer recorded for the document itself still stands when a lent face answers.
    const auto resolvedByRule = [&] {
      if (!match.lent) {
        providerDependencies_.erase(dependencyKey);
      }
    };
    if (registry_.all_of<LoadedFontComponent>(bestEntity)) {
      resolvedByRule();
      FontHandle handle(bestEntity);
      cache[cacheKey] = CachedAnswer{.font = handle};
      return handle;
    }

    if (loadFaceData(bestEntity, matchedFace(match), match.lent)) {
      resolvedByRule();
      FontHandle handle(bestEntity);
      cache[cacheKey] = CachedAnswer{.font = handle};
      return handle;
    }
    matchedSourceFailedToLoad = true;
  }

  const auto cacheUnlessRetryable = [&](FontHandle handle) {
    if (!matchedSourceFailedToLoad) {
      cache[cacheKey] = CachedAnswer{.font = handle};
    }
    return handle;
  };

  if (provider_ != nullptr && provider_->hasFamily(family)) {
    // Record the claim so later availability queries for this family skip the provider entirely.
    providerResolvedFamiliesLower_.insert(StringUtils::ToLowerAscii(family));
    return findProviderFont(family, dependencyKey, cacheKey, matchedSourceFailedToLoad);
  }

  // Fall back to the embedded Public Sans font.
  return cacheUnlessRetryable(fallbackFont());
}

bool FontManager::providerAnswerIsCurrent(std::string_view family, const ProviderFontKey& key,
                                          const CachedAnswer& answer) const {
  // Only a catalog identifies its content; other answers stay valid until the provider changes,
  // which drops every cache.
  if (provider_ == nullptr || answer.providerContentId.empty()) {
    return true;
  }
  const FontFaceRequest request{.weight = key.weight,
                                .style = static_cast<FontStyle>(key.style),
                                .stretch = static_cast<FontStretch>(key.stretch)};
  const auto current = provider_->availability(family, request);
  return current.contentId == answer.providerContentId &&
         current.contentGeneration == answer.providerGeneration;
}

void FontManager::rememberProviderDependency(const ProviderLookup& lookup, FontFaceLoadState state,
                                             FontFaceWaitReason waitReason) {
  rememberDependency(lookup.dependencyKey, lookup.request, lookup.availability, state, waitReason);
}

FontHandle FontManager::cachedProviderFont(const ProviderLookup& lookup) {
  const auto it = providerFonts_.find(lookup.contentKey);
  if (it == providerFonts_.end()) {
    return {};
  }
  if (isValidHandle(it->second) && registry_.all_of<LoadedFontComponent>(it->second.entity())) {
    rememberProviderDependency(lookup, FontFaceLoadState::Loaded);
    return it->second;
  }
  providerFonts_.erase(it);
  return {};
}

bool FontManager::awaitsExplicitResourceRefresh(const ProviderFontKey& key) const {
  if (refreshingFonts_) {
    return false;
  }
  const auto previous = providerDependencies_.find(key);
  return previous != providerDependencies_.end() &&
         (previous->second.state == FontFaceLoadState::WaitingForBytes ||
          previous->second.state == FontFaceLoadState::WaitingForAdmission);
}

bool FontManager::shouldDeferCatalogLookup(const ProviderLookup& lookup) {
  if (!lookup.isCatalog()) {
    return false;
  }
  if (const auto failure = providerFailures_.find(lookup.contentKey);
      failure != providerFailures_.end()) {
    rememberProviderDependency(lookup, FontFaceLoadState::Failed, failure->second);
    return true;
  }
  if (awaitsExplicitResourceRefresh(lookup.dependencyKey)) {
    // A running render retains its observed fallback until explicit serialized adoption.
    recordDependency(lookup.dependencyKey);
    return true;
  }
  if (lookup.availability.state != FontAssetState::Ready) {
    const bool failed = lookup.availability.state == FontAssetState::Failed ||
                        lookup.availability.state == FontAssetState::Unavailable;
    rememberProviderDependency(
        lookup, failed ? FontFaceLoadState::Failed : FontFaceLoadState::WaitingForBytes);
    return true;
  }
  return false;
}

bool FontManager::catalogFaceFitsConsumer(const ProviderLookup& lookup) {
  if (!lookup.isCatalog()) {
    return true;
  }
  const auto budget = budgetStateForRead();
  if (!providerDependencies_.contains(lookup.dependencyKey) &&
      providerDependencies_.size() >= budget->maximumFonts) {
    fontDependenciesOverflowed_ = true;
    return false;
  }
  if (lookup.availability.decodedBytes > budget->maximumBytes || budget->maximumFonts == 0) {
    providerFailures_[lookup.contentKey] = FontFaceWaitReason::RetainedBudget;
    rememberProviderDependency(lookup, FontFaceLoadState::Failed,
                               FontFaceWaitReason::RetainedBudget);
    return false;
  }
  if (lookup.availability.decodedBytes > budget->maximumBytes - budget->usedBytes ||
      budget->usedFonts >= budget->maximumFonts) {
    rememberProviderDependency(lookup, FontFaceLoadState::WaitingForAdmission,
                               FontFaceWaitReason::RetainedBudget);
    return false;
  }
  return true;
}

bool FontManager::acceptProviderAdmission(const ProviderLookup& lookup,
                                          const FontFaceAdmission& admission) {
  if (admission.state == FontFaceLoadState::Resolving) {
    return true;
  }
  const auto reason = admission.state == FontFaceLoadState::WaitingForAdmission &&
                              admission.waitReason == FontFaceWaitReason::None
                          ? FontFaceWaitReason::SharedDecodeSlot
                          : admission.waitReason;
  rememberProviderDependency(lookup, admission.state, reason);
  return false;
}

bool FontManager::providerBytesMatch(const ProviderLookup& lookup,
                                     std::span<const uint8_t> data) const {
  return !lookup.isCatalog() ||
         (lookup.availability.format == FontFileFormat::Woff2 && data.size() >= 4 &&
          readBE32(data.data()) == kWoff2Magic && data.size() == lookup.availability.encodedBytes);
}

FontHandle FontManager::loadProviderFont(const ProviderLookup& lookup,
                                         std::span<const uint8_t> data) {
  if (data.empty()) {
    return {};
  }
  bool retainedBudgetExceeded = false;
  const Entity entity = registry_.create();
  if (providerBytesMatch(lookup, data) &&
      loadFontDataIntoEntity(entity, data, FontDataTrust::Trusted,
                             lookup.isCatalog() ? &lookup.availability : nullptr,
                             &retainedBudgetExceeded)) {
    registry_.get<LoadedFontComponent>(entity).providerRequest = lookup.request;
    registry_.get<LoadedFontComponent>(entity).genericSans =
        lookup.dependencyKey.family == "sans-serif";
    registry_.get<LoadedFontComponent>(entity).immutableCatalog = lookup.isCatalog();
    const FontHandle font(entity);
    providerFonts_[lookup.contentKey] = font;
    rememberProviderDependency(lookup, FontFaceLoadState::Loaded);
    if (lookup.isCatalog()) {
      ++fontResourceRevision_;
    }
    return font;
  }
  registry_.destroy(entity);
  if (lookup.isCatalog()) {
    if (retainedBudgetExceeded) {
      rememberProviderDependency(lookup, FontFaceLoadState::WaitingForAdmission,
                                 FontFaceWaitReason::RetainedBudget);
    } else {
      providerFailures_[lookup.contentKey] = FontFaceWaitReason::None;
      rememberProviderDependency(lookup, FontFaceLoadState::Failed);
    }
  }
  return {};
}

FontHandle FontManager::cacheProviderFont(const std::string& cacheKey, FontHandle font,
                                          bool documentSourceFailed,
                                          const FontFaceAvailability& availability) {
  if (!documentSourceFailed) {
    resolutionCache()[cacheKey] =
        CachedAnswer{.font = font,
                     .providerContentId = availability.contentId,
                     .providerGeneration = availability.contentGeneration};
  }
  return font;
}

FontHandle FontManager::findProviderFont(std::string_view family,
                                         const ProviderFontKey& dependencyKey,
                                         const std::string& cacheKey, bool documentSourceFailed) {
  const FontFaceRequest request{.weight = dependencyKey.weight,
                                .style = static_cast<FontStyle>(dependencyKey.style),
                                .stretch = static_cast<FontStretch>(dependencyKey.stretch)};
  auto availability = provider_->availability(family, request);
  const ProviderLookup lookup{
      .family = family,
      .dependencyKey = dependencyKey,
      .contentKey = {dependencyKey.family, dependencyKey.weight, dependencyKey.style,
                     dependencyKey.stretch, availability.contentId, availability.contentGeneration},
      .request = request,
      .availability = std::move(availability),
  };
  if (const auto font = cachedProviderFont(lookup)) {
    return cacheProviderFont(cacheKey, font, documentSourceFailed, lookup.availability);
  }
  if (shouldDeferCatalogLookup(lookup) || !catalogFaceFitsConsumer(lookup)) {
    return fallbackFont();
  }

  {
    // Admission spans the provider copy, decode and transfer into retained ownership. The encoded
    // vector is destroyed before admission release on every path, just as on the inline path.
    auto admission = provider_->tryAcquireFace(lookup.family, lookup.request);
    if (!acceptProviderAdmission(lookup, admission)) {
      return fallbackFont();
    }
    rememberProviderDependency(lookup, FontFaceLoadState::Resolving);
    const std::vector<uint8_t> data = provider_->loadFamilyData(lookup.family, lookup.request);
    if (ScopedFontLoadInterruption::Requested()) {
      // The interrupted work discards its output; skip validating and retaining the face.
      return fallbackFont();
    }
    if (lookup.isCatalog() && data.empty()) {
      // Encoded-store eviction can race the availability read. This remains a pending face.
      rememberProviderDependency(lookup, FontFaceLoadState::WaitingForBytes);
      return fallbackFont();
    }
    if (const auto font = loadProviderFont(lookup, data)) {
      return cacheProviderFont(cacheKey, font, documentSourceFailed, lookup.availability);
    }
  }
  return fallbackFont();
}

FontHandle FontManager::loadFontData(std::span<const uint8_t> data, FontDataTrust trust) {
  return loadFontData(data, trust, {});
}

FontHandle FontManager::loadFontData(std::span<const uint8_t> data, FontDataTrust trust,
                                     const FontDataRenderingOptions& rendering) {
  const Entity entity = registry_.create();
  if (!loadFontDataIntoEntity(entity, data, trust)) {
    registry_.destroy(entity);
    return FontHandle();
  }

  auto& font = registry_.get<LoadedFontComponent>(entity);
  font.providerRequest = rendering.providerRequest;
  font.genericSans = rendering.genericSans;
  font.immutableCatalog = rendering.immutableCatalog;
  return FontHandle(entity);
}

FontLoadingDomainIdentity FontManager::fontDomainIdentity() const {
  const Registry& registry = registry_;
  const auto* context = registry.ctx().find<FontBudgetContext>();
  return context ? FontLoadingDomainIdentity{context->state,
                                             sizeof(FontBudgetState) + 4 * sizeof(void*)}
                 : FontLoadingDomainIdentity{};
}

bool FontManager::loadFontDataSharedIntoEntity(
    Entity entity, const std::shared_ptr<const std::vector<uint8_t>>& data, FontDataTrust trust,
    bool* retainedBudgetExceeded) {
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
    return finishLoad(loadWoff1(entity, *data, trust, &workLimitExceeded, retainedBudgetExceeded));
  }

  if (magic == kWoff2Magic) {
#ifdef DONNER_TEXT_WOFF2_ENABLED
    return finishLoad(
        loadWoff2(entity, *data, trust, &workLimitExceeded, nullptr, retainedBudgetExceeded));
#else
    std::cerr << "FontManager: WOFF2 font encountered but WOFF2 support not enabled. "
                 "Build with --config=text-full to enable.\n";
    return false;
#endif
  }

  // Raw TTF/OTF: share the data via shared_ptr (no copy).
  return finishLoad(
      setRawFontData(entity, data, trust, &workLimitExceeded, retainedBudgetExceeded));
}

bool FontManager::isValidationRejectedSource(
    const std::shared_ptr<const std::vector<uint8_t>>& data,
    const std::shared_ptr<FontBudgetState>& budgetState) const {
  const auto found = budgetState->validationRejectedSources.find(data.get());
  if (found == budgetState->validationRejectedSources.end()) {
    return false;
  }
  if (found->second.lock() == data) {
    return true;
  }
  budgetState->validationRejectedSources.erase(found);
  return false;
}

bool FontManager::exhaustedValidationBudgetRejects(
    std::span<const uint8_t> data, FontDataTrust trust,
    const std::shared_ptr<FontBudgetState>& budgetState) const {
  return trust == FontDataTrust::Untrusted &&
         budgetState->usedValidationWork >= budgetState->maximumValidationWork &&
         IsCompressedOrCffFont(data);
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

std::optional<FontFaceRequest> FontManager::providerFaceRequest(FontHandle handle) const {
  if (!isValidHandle(handle)) {
    return std::nullopt;
  }
  const auto* font = registry_.try_get<LoadedFontComponent>(handle.entity());
  return font ? font->providerRequest : std::nullopt;
}

bool FontManager::isGenericSansProviderFont(FontHandle handle) const {
  if (!isValidHandle(handle)) {
    return false;
  }
  const auto* font = registry_.try_get<LoadedFontComponent>(handle.entity());
  return font && font->genericSans;
}

bool FontManager::isImmutableCatalogFont(FontHandle handle) const {
  if (!isValidHandle(handle)) {
    return false;
  }
  const auto* font = registry_.try_get<LoadedFontComponent>(handle.entity());
  return font && font->immutableCatalog;
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

bool FontManager::fontsShareFamily(FontHandle first, FontHandle second) const {
  if (!isValidatedFont(first) || !isValidatedFont(second)) {
    return false;
  }
  if (first == second) {
    return true;
  }
  const auto& firstFamily = registry_.get<LoadedFontComponent>(first.entity()).actualFamily;
  const auto& secondFamily = registry_.get<LoadedFontComponent>(second.entity()).actualFamily;
  return !firstFamily.empty() && !secondFamily.empty() &&
         StringUtils::Equals<StringComparison::IgnoreCase>(firstFamily, secondFamily);
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

size_t FontManager::compressedFontDecompressionAttempts() const {
  return budgetStateForRead()->compressedFontDecompressionAttempts;
}

size_t FontManager::numValidationRejectedSources() const {
  return budgetStateForRead()->validationRejectedSources.size();
}

FontHandle FontManager::fallbackFont() {
  if (isValidHandle(fallbackHandle_)) {
    return fallbackHandle_;
  }

  // Shapes measure `ch` on every preparation; a native miss stays recorded until the font is
  // installed or retried, so do not query the resource provider again for it. Verified retained
  // bytes always win over a recorded failure, so a ready font is never skipped.
  if (embedded::kPublicSansMediumOtf.failed() && !embedded::kPublicSansMediumOtf.ready()) {
    return FontHandle();
  }

  // Keep a verified owning lease until the manager has copied its private face data.
  const ResourceLease fallback(embedded::kPublicSansMediumOtf);
  if (fallback.empty()) {
    return FontHandle();
  }

  const Entity entity = registry_.create();
  std::vector<uint8_t> data(fallback.bytes().begin(), fallback.bytes().end());
  if (!setRawFontData(entity, std::move(data), FontDataTrust::Trusted)) {
    registry_.destroy(entity);
    return FontHandle();
  }

  fallbackHandle_ = FontHandle(entity);
  return fallbackHandle_;
}

std::optional<fonts::SfntFont> FontManager::validateSfntForLoad(
    Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
    const std::shared_ptr<FontBudgetState>& budgetState, bool* validationWorkLimitExceeded,
    bool* retainedBudgetExceeded) {
  if (validationWorkLimitExceeded) {
    *validationWorkLimitExceeded = false;
  }
  if (!canStoreLoadedFont(entity, data.size(), 0, budgetState, retainedBudgetExceeded)) {
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
                                 bool* validationWorkLimitExceeded, bool* retainedBudgetExceeded) {
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  auto sfnt = validateSfntForLoad(entity, data, trust, budgetState, validationWorkLimitExceeded,
                                  retainedBudgetExceeded);
  if (!sfnt) {
    return false;
  }

  LoadedFontComponent font;
  font.ownedData = std::move(data);
  font.sfnt = std::move(*sfnt);
  font.trust = trust;
  return storeLoadedFont(entity, std::move(font), retainedBudgetExceeded);
}

bool FontManager::setRawFontData(Entity entity,
                                 std::shared_ptr<const std::vector<uint8_t>> sharedData,
                                 FontDataTrust trust, bool* validationWorkLimitExceeded,
                                 bool* retainedBudgetExceeded) {
  if (!sharedData) {
    return false;
  }
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  auto sfnt = validateSfntForLoad(entity, *sharedData, trust, budgetState,
                                  validationWorkLimitExceeded, retainedBudgetExceeded);
  if (!sfnt) {
    return false;
  }

  LoadedFontComponent font;
  font.sharedData = std::move(sharedData);
  font.sfnt = std::move(*sfnt);
  font.trust = trust;
  return storeLoadedFont(entity, std::move(font), retainedBudgetExceeded);
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

bool FontManager::storeLoadedFont(Entity entity, LoadedFontComponent font,
                                  bool* retainedBudgetExceeded) {
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  const size_t rawBytes = font.fontData().size();
  const size_t indexBytes = font.sfnt.retainedBytes();
  font.actualFamily = ActualFontFamily(font.fontData());
  const size_t familyCapacity = font.actualFamily.empty() ? 0 : font.actualFamily.capacity();
  if (familyCapacity >= budgetState->maximumBytes) {
    return false;
  }
  const size_t familyBytes = familyCapacity ? familyCapacity + 1 : 0;
  if (indexBytes > budgetState->maximumBytes - familyBytes) {
    return false;
  }
  const size_t cachedBytes = indexBytes + familyBytes;
  if (!canStoreLoadedFont(entity, rawBytes, cachedBytes, budgetState, retainedBudgetExceeded)) {
    return false;
  }
  const size_t chargeBytes = rawBytes + cachedBytes;

  if (registry_.all_of<LoadedFontComponent>(entity)) {
    registry_.remove<LoadedFontComponent>(entity);
  }
  font.reservation = FontBudgetReservation(budgetState, chargeBytes);
  registry_.emplace<LoadedFontComponent>(entity, std::move(font));
  return true;
}

bool FontManager::canStoreLoadedFont(Entity entity, size_t rawBytes, size_t indexBytes,
                                     const std::shared_ptr<FontBudgetState>& budgetState,
                                     bool* retainedBudgetExceeded) const {
  if (!FitsFontByteBudget(rawBytes, indexBytes, budgetState->maximumBytes)) {
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
    if (retainedBudgetExceeded) {
      *retainedBudgetExceeded = budgetState->maximumFonts != 0;
    }
    return false;
  }
  return true;
}

bool FontManager::loadWoff1(Entity entity, std::span<const uint8_t> data, FontDataTrust trust,
                            bool* validationWorkLimitExceeded, bool* retainedBudgetExceeded) {
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  ++budgetState->compressedFontDecompressionAttempts;
  fonts::WoffParser::Options options;
  options.maximumSfntSize = std::min(options.maximumSfntSize, budgetState->maximumBytes);
  auto maybeFont = fonts::WoffParser::Parse(data, options);
  if (maybeFont.hasError()) {
    std::cerr << "FontManager: WOFF1 parsing failed: " << maybeFont.error().reason << "\n";
    return false;
  }

  // Reconstruct sfnt byte stream from decompressed WOFF tables.
  std::vector<uint8_t> sfntData = reconstructSfnt(maybeFont.result());
  return setRawFontData(entity, std::move(sfntData), trust, validationWorkLimitExceeded,
                        retainedBudgetExceeded);
}

bool FontManager::loadFontDataIntoEntity(Entity entity, std::span<const uint8_t> data,
                                         FontDataTrust trust, const FontFaceAvailability* catalog,
                                         bool* retainedBudgetExceeded) {
  (void)catalog;
  (void)retainedBudgetExceeded;
  if (data.size() < 4) {
    return false;
  }

  const uint32_t magic = readBE32(data.data());

  if (!IsSupportedFontMagic(magic)) {
    return false;
  }

  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  if (exhaustedValidationBudgetRejects(data, trust, budgetState)) {
    return false;
  }

  if (magic == kWoffMagic) {
    return loadWoff1(entity, data, trust);
  }

  if (magic == kWoff2Magic) {
#ifdef DONNER_TEXT_WOFF2_ENABLED
    return loadWoff2(entity, data, trust, nullptr, catalog, retainedBudgetExceeded);
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
                            bool* validationWorkLimitExceeded, const FontFaceAvailability* catalog,
                            bool* retainedBudgetExceeded) {
  const std::shared_ptr<FontBudgetState> budgetState = budgetStateForWrite();
  ++budgetState->compressedFontDecompressionAttempts;
  fonts::Woff2Parser::Options options;
  options.maximumOutputSize = std::min(options.maximumOutputSize, budgetState->maximumBytes);
  if (catalog) {
    options.maximumInputSize = std::min(size_t(2 * 1024 * 1024), catalog->encodedBytes);
    options.maximumOutputSize =
        std::min({options.maximumOutputSize, size_t(2 * 1024 * 1024), catalog->decodedBytes});
    options.maximumIntermediateSize = 2 * 1024 * 1024;
    options.maximumTransformedGlyfSize = 512 * 1024;
    options.maximumTableCount = 64;
    options.expectedOutputSize = catalog->decodedBytes;
    options.maximumBrotliMemory = 8 * 1024 * 1024;
    options.requireTrueTypeOutlines = true;
  }
  auto result = fonts::Woff2Parser::Decompress(data, options);
  if (result.hasError()) {
    std::cerr << "FontManager: WOFF2 decompression failed: " << result.error().reason << "\n";
    return false;
  }

  if (catalog && result.result().size() != catalog->decodedBytes) {
    return false;
  }
  return setRawFontData(entity, std::move(result.result()), trust, validationWorkLimitExceeded,
                        retainedBudgetExceeded);
}
#endif

bool FontManager::isValidHandle(FontHandle handle) const {
  return handle && registry_.valid(handle.entity());
}

}  // namespace donner::svg
