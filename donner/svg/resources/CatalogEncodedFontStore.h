#pragma once
/// @file

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {

/// Build-pinned catalog metadata. Runtime responses never add entries or choose package paths.
struct CatalogFontAsset {
  std::string family;
  FontCategory category = FontCategory::Unknown;
  std::string contentId;  ///< SHA-256 of the encoded WOFF2 bytes, in lowercase hexadecimal.
  std::string packagePath;
  size_t encodedBytes = 0;
  size_t decodedBytes = 0;
};

/// The small, compiled catalog manifest; does not reference native embedded byte arrays.
std::span<const CatalogFontAsset> CatalogFontAssets();

/**
 * Session-owned verified encoded bytes and shared catalog decode admission.
 *
 * All methods are thread-safe. The store never fetches, decodes, or calls into a document. The
 * browser broker verifies exact length and SHA-256 before publishVerified(); the store additionally
 * enforces the compiled ID and size and only accepts an in-flight request token. A caller cannot
 * establish trust using a response-provided manifest, URL, or same-origin status.
 *
 * The wake callback is copied under the mutex and invoked after releasing it. It must capture
 * lifetime-safe application state and only schedule work. It may run after setWakeCallback({})
 * if another thread already copied it; callers must invalidate their lifetime token at teardown.
 */
class CatalogEncodedFontStore {
public:
  /// Per-asset cap for encoded buffer capacity and declared decoded bytes.
  static constexpr size_t kMaximumAssetBytes = 2 * 1024 * 1024;
  /// Maximum encoded-buffer capacity bytes retained by the store at once.
  static constexpr size_t kMaximumRetainedBytes = 4 * 1024 * 1024;

  CatalogEncodedFontStore();
  ~CatalogEncodedFontStore();
  CatalogEncodedFontStore(const CatalogEncodedFontStore&) = delete;
  CatalogEncodedFontStore& operator=(const CatalogEncodedFontStore&) = delete;

  /// Return the current consumer-visible state for a compiled catalog asset.
  /// @param contentId Compiled SHA-256 identity of the encoded font.
  FontFaceAvailability availability(std::string_view contentId) const;

  /// Queue only a coordinator-approved demand. Enumeration and provider loads do not call this.
  bool queue(std::string_view contentId);

  /// Start a queued fetch; the returned nonzero token rejects stale/duplicate completions.
  uint64_t beginFetch(std::string_view contentId);

  /// Publish broker-verified bytes for an exact active request. Returns false without mutation on
  /// unknown IDs, stale tokens, size/capacity/header mismatch, or exhausted retention budget.
  bool publishVerified(std::string_view contentId, uint64_t requestToken,
                       std::vector<uint8_t> bytes);

  /// Adopt staged immutable bytes only at a coordinator-approved idle/frame boundary. Until then
  /// availability remains Fetching and encodedBytes returns null, so a running frame cannot mix
  /// old and new font geometry. Returns whether any bytes became consumer-visible.
  bool adoptReadyAssets();

  /// Finish an exact active request with a terminal transport failure.
  bool fail(std::string_view contentId, uint64_t requestToken);

  /// Explicit retry is coordinator-owned; deadlines/cooldowns are enforced by the broker.
  bool retry(std::string_view contentId);

  /// An immutable lease preserves bytes during a provider copy. Eviction refuses leased buffers.
  std::shared_ptr<const std::vector<uint8_t>> encodedBytes(std::string_view contentId) const;
  /// Evict unleased encoded bytes for a catalog asset.
  /// @param contentId Compiled SHA-256 identity of the encoded font.
  /// @return Whether the asset's stored bytes were evicted.
  bool evict(std::string_view contentId);
  /// Encoded-buffer capacity bytes currently retained by this store.
  size_t retainedBytes() const;

  /// One admitted catalog copy/decode at a time. Release is an independent wake source, including
  /// when a pending preview document has already died and only its coordinator task remains.
  FontFaceAdmission tryAcquireDecode(std::string_view contentId) const;
  /// Revision incremented when a consumer may need to retry pending font work.
  uint64_t wakeRevision() const;
  /// Install a callback that schedules a retry after an admitted state change.
  /// @param callback Lifetime-safe callback, or empty to clear it.
  void setWakeCallback(std::function<void()> callback);

private:
  struct State;
  class Reservation;
  std::shared_ptr<State> state_;
};

}  // namespace donner::svg
