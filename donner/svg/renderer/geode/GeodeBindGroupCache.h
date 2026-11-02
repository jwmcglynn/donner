#pragma once
/// @file
/// Cross-frame cache of bind groups and whole-texture views, keyed by exact resource identity.

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
 * Entries no lookup has named for \ref kRetainedFrames frames are dropped at \ref beginFrame,
 * which the owner must call only when no frame that could have recorded work naming them is
 * still unsubmitted. Thread-safe.
 */
class GeodeBindGroupCache {
public:
  /// Frames an entry survives without being named. Pooled resources can come back in a
  /// different order from one frame to the next, so entries outlive more than one frame.
  static constexpr uint64_t kRetainedFrames = 4;

  /**
   * The bind group \p descriptor describes, created on a miss.
   *
   * @param device Device to create on a miss.
   * @param descriptor Layout and bound resources; the label is used only on a miss.
   * @return The bind group, valid until an eviction at a later \ref beginFrame, or null when the
   *   runtime refuses it.
   */
  const gpu::BindGroup* bindGroup(gpu::Device& device, gpu::BindGroupDescriptor descriptor) {
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
      found->second.lastUsedFrame = frame_;
      return &found->second.value;
    }
    gpu::Result<gpu::BindGroup> created = device.createBindGroup(std::move(descriptor));
    if (!created.hasResult()) {
      return nullptr;
    }
    auto [inserted, unused] = bindGroups_.emplace(
        std::move(key), Entry<gpu::BindGroup>{std::move(created).result(), frame_});
    (void)unused;
    return &inserted->second.value;
  }

  /**
   * A whole-texture view of \p texture, created on a miss.
   *
   * @param device Device to create on a miss.
   * @param texture Live texture to view.
   * @param label Debug label, used only on a miss.
   * @return The view, valid until an eviction at a later \ref beginFrame, or null when the
   *   runtime refuses it.
   */
  const gpu::TextureView* textureView(gpu::Device& device, const gpu::Texture& texture,
                                      RcString label) {
    const uint64_t key = IdentityKey(texture.slotIndex(), texture.generation());
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto found = views_.find(key); found != views_.end()) {
      found->second.lastUsedFrame = frame_;
      return &found->second.value;
    }
    gpu::Result<gpu::TextureView> created =
        device.createTextureView(texture, gpu::TextureViewDescriptor{std::move(label)});
    if (!created.hasResult()) {
      return nullptr;
    }
    auto [inserted, unused] =
        views_.emplace(key, Entry<gpu::TextureView>{std::move(created).result(), frame_});
    (void)unused;
    return &inserted->second.value;
  }

  /// Advance the frame and drop entries not named for \ref kRetainedFrames frames. Call only when
  /// no frame that could have recorded work naming the entries is still unsubmitted.
  void beginFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++frame_;
    const auto stale = [&](const auto& item) {
      return item.second.lastUsedFrame + kRetainedFrames < frame_;
    };
    std::erase_if(views_, stale);
    std::erase_if(bindGroups_, stale);
  }

  /// Number of cached bind groups and views.
  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return views_.size() + bindGroups_.size();
  }

private:
  static uint64_t IdentityKey(uint32_t slotIndex, uint32_t generation) {
    return (uint64_t{slotIndex} << 32) | generation;
  }

  template <typename Value>
  struct Entry {
    Value value;
    uint64_t lastUsedFrame = 0;
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

  mutable std::mutex mutex_;
  uint64_t frame_ = 0;
  // Node-based maps: lookups hand out pointers that must survive later insertions.
  std::unordered_map<uint64_t, Entry<gpu::TextureView>> views_;
  std::unordered_map<Key, Entry<gpu::BindGroup>, KeyHash> bindGroups_;
};

}  // namespace donner::geode
