#pragma once
/// @file
/// Backend-neutral bookkeeping for cached filter results: lookup, admission and LRU eviction.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "donner/svg/renderer/FilterResultCacheKey.h"

namespace donner::svg {

/**
 * Where a cached filter result is valid on the target, beyond its content.
 *
 * A filter result moves with a translation of its device transform but not with any other change,
 * so the non-translation part (\ref linear) must match exactly. Results whose edges stay sharp
 * also depend on the translation's fraction of a device pixel, which the rasterizer's coverage
 * follows: those carry the fraction's bucket, and results smooth enough to resample carry \ref
 * kAnyFraction.
 */
struct FilterResultPlacement {
  /// Bucket value for a result that is resampled at any fractional offset.
  static constexpr int kAnyFraction = -1;

  /// Device transform without its translation: `{a, b, c, d}` of the 2x3 matrix.
  std::array<double, 4> linear = {1.0, 0.0, 0.0, 1.0};
  /// Fractional translation bucket along x, or \ref kAnyFraction.
  int fractionBucketX = kAnyFraction;
  /// Fractional translation bucket along y, or \ref kAnyFraction.
  int fractionBucketY = kAnyFraction;

  /// Exact equality; a linear part that differs in any bit renders differently.
  bool operator==(const FilterResultPlacement&) const = default;
};

/**
 * Lookup, admission and least-recently-used eviction for cached filter results, generic over the
 * backend's stored result type.
 *
 * Admission keeps a continuously changing scale from recording a result that is never reused: a
 * content whose last miss, within \ref kAnimationWindowFrames, was at a different linear transform
 * is animating, and records again only once a linear transform repeats. Entries are bounded by
 * count and bytes; a backend can lower the byte bound per frame to fit a shared budget.
 *
 * @tparam Payload Backend result storage. Move-only types are supported.
 */
template <typename Payload>
class FilterResultCacheIndex {
public:
  /// Most results kept at once.
  static constexpr std::size_t kMaximumEntries = 64;
  /// Most result bytes kept at once.
  static constexpr std::uint64_t kMaximumBytes = 64ULL * 1024 * 1024;
  /// Frames within which misses at two linear transforms mark a content as animating.
  static constexpr std::uint64_t kAnimationWindowFrames = 2;
  /// Most contents whose last miss is remembered for admission.
  static constexpr std::size_t kMaximumAdmissionRecords = 64;

  /// One cached result.
  struct Entry {
    FilterResultContentKey content;   ///< What the result was rendered from.
    FilterResultPlacement placement;  ///< Where the result is valid.
    Payload payload;                  ///< Backend storage.
    std::uint64_t bytes = 0;          ///< Bytes \p payload holds.
    std::uint64_t lastUsed = 0;       ///< Use sequence of the last lookup or insertion.
  };

  /**
   * Find the result for \p content at \p placement and mark it used.
   *
   * @return The entry, or nullptr on a miss. Entries whose document is gone never match.
   */
  Entry* find(const FilterResultContentKey& content, const FilterResultPlacement& placement) {
    for (Entry& entry : entries_) {
      if (entry.placement == placement && entry.content.sameContent(content)) {
        entry.lastUsed = ++uses_;
        return &entry;
      }
    }
    return nullptr;
  }

  /**
   * Decide whether a miss for \p content at \p linear in \p frame records its result.
   *
   * @param content Content that missed.
   * @param linear Linear part of the device transform it missed at.
   * @param frame Index of the frame drawing it, nondecreasing across calls.
   */
  bool admitMiss(const FilterResultContentKey& content, const std::array<double, 4>& linear,
                 std::uint64_t frame) {
    auto record = std::find_if(admissions_.begin(), admissions_.end(),
                               [&](const Admission& a) { return a.content.sameContent(content); });
    if (record == admissions_.end()) {
      if (admissions_.size() >= kMaximumAdmissionRecords) {
        admissions_.erase(std::min_element(
            admissions_.begin(), admissions_.end(),
            [](const Admission& lhs, const Admission& rhs) { return lhs.frame < rhs.frame; }));
      }
      admissions_.push_back(Admission{content, linear, frame});
      return true;
    }
    const bool animating =
        record->linear != linear && frame - record->frame <= kAnimationWindowFrames;
    record->linear = linear;
    record->frame = frame;
    return !animating;
  }

  /**
   * Whether \p bytes more fit the count and byte bounds once least recently used entries are
   * evicted, without evicting.
   */
  [[nodiscard]] bool canHold(std::uint64_t bytes) const { return bytes <= byteLimit_; }

  /**
   * Insert a result, evicting least recently used entries until it fits. The caller has checked
   * \ref canHold.
   *
   * @return Payloads of the evicted entries, for the backend to retire.
   */
  std::vector<Payload> insert(FilterResultContentKey content, FilterResultPlacement placement,
                              Payload payload, std::uint64_t bytes) {
    std::vector<Payload> evicted = evictUntil(kMaximumEntries - 1, byteLimit_ - bytes);
    entries_.push_back(Entry{std::move(content), placement, std::move(payload), bytes, ++uses_});
    bytes_ += bytes;
    return evicted;
  }

  /**
   * Lower the byte bound to \p limit (never above \ref kMaximumBytes), evicting least recently
   * used entries until the held bytes fit.
   *
   * @return Payloads of the evicted entries.
   */
  std::vector<Payload> setByteLimit(std::uint64_t limit) {
    byteLimit_ = std::min(limit, kMaximumBytes);
    return evictUntil(kMaximumEntries, byteLimit_);
  }

  /// Drop entries whose document is gone. @return Their payloads.
  std::vector<Payload> dropExpired() {
    std::vector<Payload> dropped;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->content.document.expired()) {
        bytes_ -= it->bytes;
        dropped.push_back(std::move(it->payload));
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    std::erase_if(admissions_, [](const Admission& a) { return a.content.document.expired(); });
    return dropped;
  }

  /// Drop every entry and admission record. @return The entries' payloads.
  std::vector<Payload> clear() {
    std::vector<Payload> dropped;
    dropped.reserve(entries_.size());
    for (Entry& entry : entries_) {
      dropped.push_back(std::move(entry.payload));
    }
    entries_.clear();
    admissions_.clear();
    bytes_ = 0;
    return dropped;
  }

  /// Number of results held.
  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  /// Bytes the held results occupy.
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }
  /// Current byte bound.
  [[nodiscard]] std::uint64_t byteLimit() const { return byteLimit_; }

private:
  struct Admission {
    FilterResultContentKey content;
    std::array<double, 4> linear;
    std::uint64_t frame = 0;
  };

  std::vector<Payload> evictUntil(std::size_t maximumEntries, std::uint64_t maximumBytes) {
    std::vector<Payload> evicted;
    while (!entries_.empty() && (entries_.size() > maximumEntries || bytes_ > maximumBytes)) {
      auto victim = std::min_element(
          entries_.begin(), entries_.end(),
          [](const Entry& lhs, const Entry& rhs) { return lhs.lastUsed < rhs.lastUsed; });
      bytes_ -= victim->bytes;
      evicted.push_back(std::move(victim->payload));
      entries_.erase(victim);
    }
    return evicted;
  }

  std::vector<Entry> entries_;
  std::vector<Admission> admissions_;
  std::uint64_t bytes_ = 0;
  std::uint64_t byteLimit_ = kMaximumBytes;
  std::uint64_t uses_ = 0;
};

}  // namespace donner::svg
