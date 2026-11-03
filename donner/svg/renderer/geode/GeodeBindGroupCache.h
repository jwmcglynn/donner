#pragma once
/// @file
/// Cross-frame cache of bind groups and whole-texture views, keyed by exact resource identity.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/Handles.h"

namespace donner::geode {

/**
 * Bind groups and texture views kept across frames, so a steady frame that binds the same
 * resources as the last one creates none.
 *
 * Every key is built from resource identities (slot plus generation): a bind group key holds its
 * layout and, per entry, the binding index, the bound resource and, for a buffer, its range; a
 * view key holds its texture. A slot the runtime reuses for a different resource carries a new
 * generation, so a hit always names the very same objects the cached entry was built from. The
 * contents of a bound buffer or texture are free to change between frames; a bind group only
 * names them.
 *
 * Lifetime follows the device's frame generations. Every lookup stamps its entry with a
 * generation at or after that of the frame recording the use (callers pass the device's latest
 * generation). An entry stamped before the oldest open generation was last named by frames that
 * have all closed, so it may be released. \ref evict releases such entries once they are also
 * \ref kRetainedGenerations old.
 *
 * The live-entry cap is soft. An insertion past it retires the least recently stamped entries
 * that no open frame named, and only those: an entry stamped at or after the oldest open
 * generation stays live even past the cap, until that frame closes and the entry ages out.
 * Retired entries keep their map nodes (extracted, not erased), so a pointer handed out earlier
 * stays valid until a later \ref evict releases them, which it does once no open frame can name
 * them. How long that takes, and so how large the retired list grows, depends on how long the
 * oldest open frame stays open.
 *
 * Thread-safe; the frame generations it is given come from GeodeDevice, whose frame bookkeeping
 * requires that all renderers on one device run on one thread.
 */
class GeodeBindGroupCache {
public:
  /// Generations an entry survives without being named, so pooled resources that come back in a
  /// different order from one frame to the next still hit.
  static constexpr uint64_t kRetainedGenerations = 8;

  /// Live entries (views plus bind groups) kept before the least recently stamped are retired.
  static constexpr std::size_t kMaximumEntries = 4096;

  /**
   * The bind group \p descriptor describes, created on a miss.
   *
   * @param device Device to create on a miss.
   * @param descriptor Layout and bound resources; the label is used only on a miss.
   * @param useGeneration A frame generation at or after that of the frame recording this use.
   * @param oldestOpenGeneration The device's oldest open frame generation, `~0` when none is open.
   *   Only entries stamped before it may be retired to make room.
   * @return The bind group, valid at least until that frame has closed, or null when the runtime
   *   refuses it.
   */
  const gpu::BindGroup* bindGroup(gpu::Device& device, gpu::BindGroupDescriptor descriptor,
                                  uint64_t useGeneration, uint64_t oldestOpenGeneration) {
    Key key;
    key.words.reserve(1 + descriptor.entries.size() * 4);
    key.words.push_back(IdentityKey(descriptor.layout.slotIndex(), descriptor.layout.generation()));
    for (const gpu::BindGroupEntry& entry : descriptor.entries) {
      key.words.push_back((uint64_t{entry.binding} << 8) | entry.resource.index());
      std::visit(
          [&](const auto& resource) {
            using Resource = std::decay_t<decltype(resource)>;
            if constexpr (std::is_same_v<Resource, gpu::BufferBinding>) {
              key.words.push_back(
                  IdentityKey(resource.buffer.slotIndex(), resource.buffer.generation()));
              key.words.push_back(resource.offsetBytes);
              key.words.push_back(resource.sizeBytes);
            } else if constexpr (std::is_same_v<Resource, gpu::TextureViewBinding>) {
              key.words.push_back(
                  IdentityKey(resource.view.slotIndex(), resource.view.generation()));
            } else {
              key.words.push_back(
                  IdentityKey(resource.sampler.slotIndex(), resource.sampler.generation()));
            }
          },
          entry.resource);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (auto found = bindGroups_.find(key); found != bindGroups_.end()) {
      found->second.stamp = std::max(found->second.stamp, useGeneration);
      return &found->second.value;
    }
    gpu::Result<gpu::BindGroup> created = device.createBindGroup(std::move(descriptor));
    if (!created.hasResult()) {
      return nullptr;
    }
    retireOverCapacityLocked(std::min(oldestOpenGeneration, useGeneration));
    auto [inserted, unused] = bindGroups_.emplace(
        std::move(key), Entry<gpu::BindGroup>{std::move(created).result(), useGeneration});
    (void)unused;
    return &inserted->second.value;
  }

  /**
   * A whole-texture view of \p texture, created on a miss.
   *
   * @param device Device to create on a miss.
   * @param texture Live texture to view.
   * @param label Debug label, used only on a miss.
   * @param useGeneration A frame generation at or after that of the frame recording this use.
   * @param oldestOpenGeneration The device's oldest open frame generation, `~0` when none is open.
   *   Only entries stamped before it may be retired to make room.
   * @return The view, valid at least until that frame has closed, or null when the runtime
   *   refuses it.
   */
  const gpu::TextureView* textureView(gpu::Device& device, const gpu::Texture& texture,
                                      RcString label, uint64_t useGeneration,
                                      uint64_t oldestOpenGeneration) {
    const uint64_t key = IdentityKey(texture.slotIndex(), texture.generation());
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto found = views_.find(key); found != views_.end()) {
      found->second.stamp = std::max(found->second.stamp, useGeneration);
      return &found->second.value;
    }
    gpu::Result<gpu::TextureView> created =
        device.createTextureView(texture, gpu::TextureViewDescriptor{std::move(label)});
    if (!created.hasResult()) {
      return nullptr;
    }
    retireOverCapacityLocked(std::min(oldestOpenGeneration, useGeneration));
    auto [inserted, unused] =
        views_.emplace(key, Entry<gpu::TextureView>{std::move(created).result(), useGeneration});
    (void)unused;
    return &inserted->second.value;
  }

  /**
   * Release what no open frame can name: retired entries stamped before \p oldestOpenGeneration,
   * and live entries stamped before it that are also \ref kRetainedGenerations older than
   * \p latestGeneration.
   *
   * @param oldestOpenGeneration The device's oldest open frame generation, `~0` when none is open.
   * @param latestGeneration The device's most recently minted frame generation.
   */
  void evict(uint64_t oldestOpenGeneration, uint64_t latestGeneration) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto releasable = [&](uint64_t stamp) {
      return stamp < oldestOpenGeneration && stamp + kRetainedGenerations <= latestGeneration;
    };
    std::erase_if(views_, [&](const auto& item) { return releasable(item.second.stamp); });
    std::erase_if(bindGroups_, [&](const auto& item) { return releasable(item.second.stamp); });
    std::erase_if(retiredViews_,
                  [&](const auto& node) { return node.mapped().stamp < oldestOpenGeneration; });
    std::erase_if(retiredBindGroups_,
                  [&](const auto& node) { return node.mapped().stamp < oldestOpenGeneration; });
  }

  /// Number of live (lookup-reachable) bind groups and views.
  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return views_.size() + bindGroups_.size();
  }

  /// Number of retired entries still held for frames that may name them.
  [[nodiscard]] std::size_t retiredSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retiredViews_.size() + retiredBindGroups_.size();
  }

  /// Replace the live-entry cap of \ref kMaximumEntries, for tests.
  void setMaximumEntriesForTesting(std::size_t maximumEntries) {
    std::lock_guard<std::mutex> lock(mutex_);
    maximumEntries_ = std::max<std::size_t>(maximumEntries, 1);
  }

private:
  static uint64_t IdentityKey(uint32_t slotIndex, uint32_t generation) {
    return (uint64_t{slotIndex} << 32) | generation;
  }

  template <typename Value>
  struct Entry {
    Value value;
    uint64_t stamp = 0;  ///< Latest generation that named the entry.
  };

  struct Key {
    std::vector<uint64_t> words;
    bool operator==(const Key& other) const = default;
  };

  struct KeyHash {
    std::size_t operator()(const Key& key) const {
      uint64_t hash = 1469598103934665603ull;
      for (uint64_t word : key.words) {
        hash = (hash ^ word) * 1099511628211ull;
      }
      return static_cast<std::size_t>(hash);
    }
  };

  /// Before an insertion that would exceed the cap, retire the least recently stamped live
  /// entries stamped before \p retirableBefore, down to three quarters of the cap so a churning
  /// frame pays the scan rarely. Entries stamped at or after it may be named by an open frame and
  /// stay live, so the cap is exceeded until they age out. Retiring extracts the map node, so
  /// pointers into it stay valid until \ref evict releases it.
  void retireOverCapacityLocked(uint64_t retirableBefore) {
    const std::size_t live = views_.size() + bindGroups_.size();
    if (live < maximumEntries_) {
      return;
    }
    const std::size_t target = maximumEntries_ - maximumEntries_ / 4;
    std::vector<uint64_t> stamps;
    stamps.reserve(live);
    for (const auto& item : views_) {
      if (item.second.stamp < retirableBefore) {
        stamps.push_back(item.second.stamp);
      }
    }
    for (const auto& item : bindGroups_) {
      if (item.second.stamp < retirableBefore) {
        stamps.push_back(item.second.stamp);
      }
    }
    const std::size_t retireCount = std::min(stamps.size(), live - std::min(live, target));
    if (retireCount == 0) {
      return;
    }
    std::nth_element(stamps.begin(), stamps.begin() + static_cast<std::ptrdiff_t>(retireCount - 1),
                     stamps.end());
    const uint64_t cutoff = stamps[retireCount - 1];
    std::size_t retired = 0;
    for (auto it = views_.begin(); it != views_.end() && retired < retireCount;) {
      const auto current = it++;
      if (current->second.stamp <= cutoff && current->second.stamp < retirableBefore) {
        retiredViews_.push_back(views_.extract(current));
        ++retired;
      }
    }
    for (auto it = bindGroups_.begin(); it != bindGroups_.end() && retired < retireCount;) {
      const auto current = it++;
      if (current->second.stamp <= cutoff && current->second.stamp < retirableBefore) {
        retiredBindGroups_.push_back(bindGroups_.extract(current));
        ++retired;
      }
    }
  }

  mutable std::mutex mutex_;
  std::size_t maximumEntries_ = kMaximumEntries;
  // Node-based maps: lookups hand out pointers that must survive later insertions.
  using ViewMap = std::unordered_map<uint64_t, Entry<gpu::TextureView>>;
  using BindGroupMap = std::unordered_map<Key, Entry<gpu::BindGroup>, KeyHash>;
  ViewMap views_;
  BindGroupMap bindGroups_;
  // Extracted nodes: each keeps the allocation a handed-out pointer points into.
  std::vector<ViewMap::node_type> retiredViews_;
  std::vector<BindGroupMap::node_type> retiredBindGroups_;
};

}  // namespace donner::geode
