#include "donner/svg/resources/CatalogEncodedFontStore.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace donner::svg {
namespace {

bool HasCatalogWoff2Header(std::span<const uint8_t> bytes) {
  return bytes.size() >= 48 && bytes[0] == 'w' && bytes[1] == 'O' && bytes[2] == 'F' &&
         bytes[3] == '2';
}

}  // namespace

struct CatalogEncodedFontStore::State {
  struct Entry {
    CatalogFontAsset asset;
    FontAssetState state = FontAssetState::Absent;
    uint64_t requestToken = 0;
    bool staged = false;
    std::shared_ptr<const std::vector<uint8_t>> bytes;

    bool matchesActiveRequest(uint64_t token) const {
      return state == FontAssetState::Fetching && token != 0 && requestToken == token && !staged;
    }

    bool matchesEncodedPayload(std::span<const uint8_t> payload) const {
      if (payload.size() != asset.encodedBytes || !HasCatalogWoff2Header(payload) ||
          asset.decodedBytes > kMaximumAssetBytes)
        return false;
      const size_t declaredBytes = (size_t(payload[16]) << 24) | (size_t(payload[17]) << 16) |
                                   (size_t(payload[18]) << 8) | size_t(payload[19]);
      return declaredBytes == asset.decodedBytes;
    }
  };

  mutable std::mutex mutex;
  std::vector<Entry> entries;
  size_t retainedBytes = 0;
  uint64_t nextRequestToken = 1;
  uint64_t wakeRevision = 0;
  bool decodeAdmitted = false;
  std::function<void()> wake;

  auto find(std::string_view id) {
    return std::find_if(entries.begin(), entries.end(),
                        [id](const Entry& entry) { return entry.asset.contentId == id; });
  }

  bool canRetain(const std::vector<uint8_t>& bytes) const {
    return bytes.capacity() <= kMaximumAssetBytes &&
           bytes.capacity() <= kMaximumRetainedBytes - retainedBytes;
  }

  std::function<void()> changed() {
    ++wakeRevision;
    return wake;
  }
};

class CatalogEncodedFontStore::Reservation final : public FontFaceLoadReservation {
public:
  explicit Reservation(std::shared_ptr<State> state) : state_(std::move(state)) {}
  ~Reservation() override {
    std::function<void()> wake;
    {
      const std::lock_guard lock(state_->mutex);
      state_->decodeAdmitted = false;
      wake = state_->changed();
    }
    if (wake) wake();
  }

private:
  std::shared_ptr<State> state_;
};

CatalogEncodedFontStore::CatalogEncodedFontStore() : state_(std::make_shared<State>()) {
  for (const auto& asset : CatalogFontAssets()) {
    state_->entries.push_back({.asset = asset});
  }
}

CatalogEncodedFontStore::~CatalogEncodedFontStore() = default;

FontFaceAvailability CatalogEncodedFontStore::availability(std::string_view contentId) const {
  const std::lock_guard lock(state_->mutex);
  const auto it = state_->find(contentId);
  if (it == state_->entries.end()) return {};
  return {.state = it->state,
          .format = FontFileFormat::Woff2,
          .contentId = it->asset.contentId,
          // Immutable hashes keep the same identity across eviction and refill.
          .contentGeneration = 1,
          .encodedBytes = it->asset.encodedBytes,
          .decodedBytes = it->asset.decodedBytes};
}

bool CatalogEncodedFontStore::queue(std::string_view contentId) {
  const std::lock_guard lock(state_->mutex);
  const auto it = state_->find(contentId);
  if (it == state_->entries.end() || it->state != FontAssetState::Absent) return false;
  it->state = FontAssetState::Queued;
  return true;
}

uint64_t CatalogEncodedFontStore::beginFetch(std::string_view contentId) {
  const std::lock_guard lock(state_->mutex);
  const auto it = state_->find(contentId);
  if (it == state_->entries.end() || it->state != FontAssetState::Queued) return 0;
  const auto fetching =
      std::count_if(state_->entries.begin(), state_->entries.end(), [](const State::Entry& entry) {
        return entry.state == FontAssetState::Fetching && !entry.staged;
      });
  if (fetching >= 2) return 0;
  it->state = FontAssetState::Fetching;
  it->requestToken = state_->nextRequestToken++;
  return it->requestToken;
}

bool CatalogEncodedFontStore::publishVerified(std::string_view contentId, uint64_t requestToken,
                                              std::vector<uint8_t> bytes) {
  std::function<void()> wake;
  {
    const std::lock_guard lock(state_->mutex);
    const auto it = state_->find(contentId);
    if (it == state_->entries.end() || !it->matchesActiveRequest(requestToken) ||
        !it->matchesEncodedPayload(bytes) || !state_->canRetain(bytes)) {
      return false;
    }
    state_->retainedBytes += bytes.capacity();
    it->bytes = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    it->staged = true;
    wake = state_->changed();
  }
  if (wake) wake();
  return true;
}

bool CatalogEncodedFontStore::adoptReadyAssets() {
  std::function<void()> wake;
  bool adopted = false;
  {
    const std::lock_guard lock(state_->mutex);
    for (auto& entry : state_->entries) {
      if (entry.staged) {
        entry.state = FontAssetState::Ready;
        entry.staged = false;
        adopted = true;
      }
    }
    if (adopted) wake = state_->changed();
  }
  if (wake) wake();
  return adopted;
}

bool CatalogEncodedFontStore::fail(std::string_view contentId, uint64_t requestToken) {
  std::function<void()> wake;
  {
    const std::lock_guard lock(state_->mutex);
    const auto it = state_->find(contentId);
    if (it == state_->entries.end() || !it->matchesActiveRequest(requestToken)) return false;
    it->state = FontAssetState::Failed;
    wake = state_->changed();
  }
  if (wake) wake();
  return true;
}

bool CatalogEncodedFontStore::retry(std::string_view contentId) {
  const std::lock_guard lock(state_->mutex);
  const auto it = state_->find(contentId);
  if (it == state_->entries.end() || it->state != FontAssetState::Failed) return false;
  it->state = FontAssetState::Queued;
  return true;
}

std::shared_ptr<const std::vector<uint8_t>> CatalogEncodedFontStore::encodedBytes(
    std::string_view contentId) const {
  const std::lock_guard lock(state_->mutex);
  const auto it = state_->find(contentId);
  return it == state_->entries.end() || it->state != FontAssetState::Ready ? nullptr : it->bytes;
}

bool CatalogEncodedFontStore::evict(std::string_view contentId) {
  const std::lock_guard lock(state_->mutex);
  const auto it = state_->find(contentId);
  if (it == state_->entries.end() || it->state != FontAssetState::Ready || !it->bytes ||
      it->bytes.use_count() != 1)
    return false;
  state_->retainedBytes -= it->bytes->capacity();
  it->bytes.reset();
  it->state = FontAssetState::Absent;
  return true;
}

size_t CatalogEncodedFontStore::retainedBytes() const {
  const std::lock_guard lock(state_->mutex);
  return state_->retainedBytes;
}

FontFaceAdmission CatalogEncodedFontStore::tryAcquireDecode(std::string_view contentId) const {
  const std::lock_guard lock(state_->mutex);
  const auto it = state_->find(contentId);
  if (it == state_->entries.end() || it->asset.encodedBytes > kMaximumAssetBytes ||
      it->asset.decodedBytes > kMaximumAssetBytes) {
    return {.state = FontFaceLoadState::Failed};
  }
  if (state_->decodeAdmitted)
    return {.state = FontFaceLoadState::WaitingForAdmission,
            .waitReason = FontFaceWaitReason::SharedDecodeSlot};
  auto reservation = std::make_unique<Reservation>(state_);
  state_->decodeAdmitted = true;
  return {.state = FontFaceLoadState::Resolving, .reservation = std::move(reservation)};
}

uint64_t CatalogEncodedFontStore::wakeRevision() const {
  const std::lock_guard lock(state_->mutex);
  return state_->wakeRevision;
}

void CatalogEncodedFontStore::setWakeCallback(std::function<void()> callback) {
  const std::lock_guard lock(state_->mutex);
  state_->wake = std::move(callback);
}

}  // namespace donner::svg
