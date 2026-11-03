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
 * follows: those carry the fraction's bucket (a quarter of a pixel), and results smooth enough to
 * resample carry \ref kAnyFraction.
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
 * Admission is two-touch: a content records a result only on its second miss at the same linear
 * transform within \ref kAdmissionWindowFrames, so contents whose keys never repeat (a scale that
 * keeps changing, inputs that change every draw) record nothing. An insertion that would evict a
 * result used in the current or previous frame is refused, so more contents than the cache holds
 * do not evict each other every frame.
 *
 * A result inserted during a frame is pending: lookups find it, so later draws in the same frame
 * reuse it, but it becomes durable only when the backend commits the frame's results after the
 * frame's work is submitted, and is dropped if the frame is abandoned.
 *
 * Frame numbers are the caller's own, nondecreasing across calls.
 *
 * @tparam Payload Backend result storage. Move-only types are supported.
 */
template <typename Payload>
class FilterResultCacheIndex {
public:
  /// Most results kept at once.
  static constexpr std::size_t kMaximumEntries = 256;
  /// Most result bytes kept at once.
  static constexpr std::uint64_t kMaximumBytes = 64ULL * 1024 * 1024;
  /// Frames within which a second miss at a linear transform admits the content.
  static constexpr std::uint64_t kAdmissionWindowFrames = 120;
  /// Most contents whose recent misses are remembered for admission.
  static constexpr std::size_t kMaximumAdmissionRecords = 512;
  /// Most linear transforms remembered per content.
  static constexpr std::size_t kLinearsPerContent = 4;

  /// One cached result.
  struct Entry {
    FilterResultContentKey content;   ///< What the result was rendered from.
    FilterResultPlacement placement;  ///< Where the result is valid.
    Payload payload;                  ///< Backend storage.
    std::uint64_t bytes = 0;          ///< Bytes \p payload holds.
    std::uint64_t lastUsed = 0;       ///< Use sequence of the last lookup or insertion.
    std::uint64_t lastUsedFrame = 0;  ///< Frame of the last lookup or insertion.
    bool pending = false;             ///< Inserted in a frame whose work is not yet submitted.
  };

  /**
   * Find the result for \p content at \p placement and mark it used in \p frame.
   *
   * @return The entry, or nullptr on a miss. Entries whose document is gone never match.
   */
  Entry* find(const FilterResultContentKey& content, const FilterResultPlacement& placement,
              std::uint64_t frame) {
    for (Entry& entry : entries_) {
      if (entry.placement == placement && entry.content.sameContent(content)) {
        entry.lastUsed = ++uses_;
        entry.lastUsedFrame = frame;
        return &entry;
      }
    }
    return nullptr;
  }

  /**
   * Decide whether a miss for \p content at \p linear in \p frame records its result: true when
   * the same content already missed at the same linear transform within the admission window.
   */
  bool admitMiss(const FilterResultContentKey& content, const std::array<double, 4>& linear,
                 std::uint64_t frame) {
    auto record = std::find_if(admissions_.begin(), admissions_.end(),
                               [&](const Admission& a) { return a.content.sameContent(content); });
    if (record == admissions_.end()) {
      if (admissions_.size() >= kMaximumAdmissionRecords) {
        admissions_.erase(std::min_element(admissions_.begin(), admissions_.end(),
                                           [](const Admission& lhs, const Admission& rhs) {
                                             return lhs.lastFrame() < rhs.lastFrame();
                                           }));
      }
      admissions_.push_back(Admission{content, {}});
      record = admissions_.end() - 1;
    }
    auto seen = std::find_if(record->linears.begin(), record->linears.end(),
                             [&](const SeenLinear& s) { return s.linear == linear; });
    const bool admitted =
        seen != record->linears.end() && frame - seen->frame <= kAdmissionWindowFrames;
    if (seen == record->linears.end()) {
      if (record->linears.size() >= kLinearsPerContent) {
        record->linears.erase(std::min_element(
            record->linears.begin(), record->linears.end(),
            [](const SeenLinear& lhs, const SeenLinear& rhs) { return lhs.frame < rhs.frame; }));
      }
      record->linears.push_back(SeenLinear{linear, frame});
    } else {
      seen->frame = frame;
    }
    return admitted;
  }

  /**
   * Whether a result of \p bytes can be inserted in \p frame: it fits the byte bound, and making
   * room evicts nothing used in this frame or the previous one.
   */
  [[nodiscard]] bool canInsert(std::uint64_t bytes, std::uint64_t frame) const {
    if (bytes > byteLimit_) {
      return false;
    }
    std::vector<const Entry*> order;
    order.reserve(entries_.size());
    for (const Entry& entry : entries_) {
      order.push_back(&entry);
    }
    std::sort(order.begin(), order.end(),
              [](const Entry* lhs, const Entry* rhs) { return lhs->lastUsed < rhs->lastUsed; });
    std::size_t count = entries_.size();
    std::uint64_t held = bytes_;
    for (const Entry* victim : order) {
      if (count < kMaximumEntries && held + bytes <= byteLimit_) {
        break;
      }
      if (victim->lastUsedFrame + 1 >= frame) {
        return false;
      }
      --count;
      held -= victim->bytes;
    }
    return count < kMaximumEntries && held + bytes <= byteLimit_;
  }

  /**
   * Insert a pending result in \p frame, evicting least recently used entries until it fits. The
   * caller has checked \ref canInsert.
   *
   * @return Payloads of the evicted entries, for the backend to retire.
   */
  std::vector<Payload> insert(FilterResultContentKey content, FilterResultPlacement placement,
                              Payload payload, std::uint64_t bytes, std::uint64_t frame) {
    std::vector<Payload> evicted = evictUntil(kMaximumEntries - 1, byteLimit_ - bytes);
    entries_.push_back(Entry{std::move(content), placement, std::move(payload), bytes, ++uses_,
                             frame, /*pending=*/true});
    bytes_ += bytes;
    return evicted;
  }

  /// Make every pending result durable, once the frame that recorded it is submitted.
  void commitPending() {
    for (Entry& entry : entries_) {
      entry.pending = false;
    }
  }

  /// Drop every pending result, when the frame that recorded it is abandoned.
  /// @return Their payloads.
  std::vector<Payload> dropPending() {
    std::vector<Payload> dropped;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->pending) {
        bytes_ -= it->bytes;
        dropped.push_back(std::move(it->payload));
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    return dropped;
  }

  /**
   * Set the byte bound to \p limit (never above \ref kMaximumBytes), evicting least recently used
   * entries until the held bytes fit.
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

  /// Number of held results for which \p predicate holds on their content key and placement.
  template <typename Predicate>
  [[nodiscard]] std::size_t countIf(const Predicate& predicate) const {
    return static_cast<std::size_t>(std::count_if(
        entries_.begin(), entries_.end(),
        [&](const Entry& entry) { return predicate(entry.content, entry.placement); }));
  }

  /// Bytes of the held results for which \p predicate holds on their content key and placement.
  template <typename Predicate>
  [[nodiscard]] std::uint64_t bytesIf(const Predicate& predicate) const {
    std::uint64_t total = 0;
    for (const Entry& entry : entries_) {
      if (predicate(entry.content, entry.placement)) {
        total += entry.bytes;
      }
    }
    return total;
  }

  /**
   * Evict the least recently used result for which \p predicate holds, unless it was used in
   * \p frame or the frame before, so a quota of one kind of result turns over without evicting
   * results the current frames draw.
   *
   * @return The evicted payload, or nullopt when no such result may be evicted.
   */
  template <typename Predicate>
  std::optional<Payload> evictStale(const Predicate& predicate, std::uint64_t frame) {
    auto victim = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (predicate(it->content, it->placement) && it->lastUsedFrame + 1 < frame &&
          (victim == entries_.end() || it->lastUsed < victim->lastUsed)) {
        victim = it;
      }
    }
    if (victim == entries_.end()) {
      return std::nullopt;
    }
    bytes_ -= victim->bytes;
    std::optional<Payload> evicted(std::move(victim->payload));
    entries_.erase(victim);
    return evicted;
  }

  /// Number of results held, pending ones included.
  [[nodiscard]] std::size_t size() const { return entries_.size(); }
  /// Bytes the held results occupy.
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }
  /// Current byte bound.
  [[nodiscard]] std::uint64_t byteLimit() const { return byteLimit_; }

private:
  struct SeenLinear {
    std::array<double, 4> linear;
    std::uint64_t frame = 0;
  };

  struct Admission {
    FilterResultContentKey content;
    std::vector<SeenLinear> linears;

    std::uint64_t lastFrame() const {
      std::uint64_t last = 0;
      for (const SeenLinear& seen : linears) {
        last = std::max(last, seen.frame);
      }
      return last;
    }
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
