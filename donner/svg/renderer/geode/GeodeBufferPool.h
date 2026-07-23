#pragma once
/// @file
/// Frame-transient GPU buffer pool for GeoEncoder arenas (design doc 0030).
///
/// `GeoEncoder` instances are recreated every frame (and per layer /
/// filter / mask push), so without pooling every frame re-creates its
/// bump-arena buffers from scratch: 9-20 `createBuffer` driver
/// allocations per steady-state frame, plus the matching releases. This
/// pool lets a frame's encoders reuse the buffers grown by earlier
/// frames, driving steady-state `bufferCreates` toward zero.
///
/// Safety: buffers are released into the pool by `~GeoEncoder`, which by
/// the renderer's existing lifetime discipline only runs after the
/// commands referencing those buffers have been submitted (shared-mode
/// encoders are parked in `frameFinishedEncoders` until the frame's
/// single submit; own-CommandEncoder encoders submit in `finish()`).
/// With the donner::gpu runtime this discipline is mandatory-by-validation:
/// `gpu::Device::submit` re-validates every recorded buffer identity and
/// fails closed if a handle was destroyed, so an early release would be a
/// loud submit failure, not a silent use-after-free. Re-acquiring a pooled
/// buffer on a later frame is safe: `writeBuffer` is queue-ordered after
/// previously submitted work.

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/Handles.h"

namespace donner::geode {

/**
 * Size-and-usage-keyed free list of donner::gpu buffers.
 *
 * Owned by `RendererGeode::Impl` (mirrors the M4.2 transient
 * render-target texture pool) and handed to every `GeoEncoder` the
 * renderer constructs. Not thread-safe; all use is on the renderer's
 * thread.
 */
class GeodeBufferPool {
public:
  /**
   * Acquire a pooled buffer with exactly `usage` and capacity >=
   * `minCapacity`.
   *
   * Preference order: an entry released under the same `label` (each
   * arena reacquires its own previous, right-sized buffer, so a Slug
   * curve arena never trades buffers with a tiny grid arena of the same
   * usage class), else the smallest usage-compatible entry.
   *
   * @param usage Required buffer usage flags (exact match).
   * @param label Arena debug label (string literal; matched by content).
   * @param minCapacity Minimum byte capacity.
   * @param outCapacity Set to the returned buffer's capacity on success.
   * @return The buffer, or a null handle if the pool has no match.
   */
  gpu::Buffer acquire(gpu::BufferUsage usage, const char* label, uint64_t minCapacity,
                      uint64_t* outCapacity) {
    size_t bestIndex = entries_.size();
    bool bestLabelMatch = false;
    for (size_t i = 0; i < entries_.size(); ++i) {
      const Entry& entry = entries_[i];
      if (entry.usage != usage || entry.capacity < minCapacity) {
        continue;
      }
      const bool labelMatch = label != nullptr && entry.label != nullptr &&
                              (entry.label == label || std::strcmp(entry.label, label) == 0);
      if (bestIndex == entries_.size() || (labelMatch && !bestLabelMatch) ||
          (labelMatch == bestLabelMatch && entry.capacity < entries_[bestIndex].capacity)) {
        bestIndex = i;
        bestLabelMatch = labelMatch;
      }
    }
    if (bestIndex == entries_.size()) {
      return {};
    }
    gpu::Buffer buffer = std::move(entries_[bestIndex].buffer);
    if (outCapacity != nullptr) {
      *outCapacity = entries_[bestIndex].capacity;
    }
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(bestIndex));
    return buffer;
  }

  /**
   * Return a buffer to the pool. When the pool is full, the smallest
   * entry (including the incoming buffer) is dropped instead, bounding
   * pooled VRAM while keeping the large arenas that are expensive to
   * re-grow.
   */
  void release(gpu::Buffer buffer, gpu::BufferUsage usage, const char* label, uint64_t capacity) {
    if (!buffer.isValid()) {
      return;
    }
    if (entries_.size() >= kMaxEntries) {
      // Find the smallest pooled entry; keep the incoming buffer only if
      // it is larger.
      size_t smallest = 0;
      for (size_t i = 1; i < entries_.size(); ++i) {
        if (entries_[i].capacity < entries_[smallest].capacity) {
          smallest = i;
        }
      }
      if (entries_[smallest].capacity >= capacity) {
        return;  // Incoming buffer is the smallest; drop it.
      }
      entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(smallest));
    }
    entries_.push_back(Entry{std::move(buffer), usage, label, capacity});
  }

  /// Number of pooled buffers (for tests).
  [[nodiscard]] size_t size() const { return entries_.size(); }

  /// Drop every pooled buffer.
  void clear() { entries_.clear(); }

private:
  struct Entry {
    gpu::Buffer buffer;
    gpu::BufferUsage usage = gpu::BufferUsage::None;
    const char* label = nullptr;  ///< Arena label (string literal).
    uint64_t capacity = 0;
  };

  /// Steady-state frames use ~10-20 arena buffers; 64 leaves headroom
  /// for layer-heavy frames without letting a pathological frame pin
  /// unbounded VRAM.
  static constexpr size_t kMaxEntries = 64;

  std::vector<Entry> entries_;
};

}  // namespace donner::geode
