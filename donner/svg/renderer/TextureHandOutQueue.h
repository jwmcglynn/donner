#pragma once
/// @file
/// Free-list ordering for the Geode transient texture pool.

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>

namespace donner::svg {

/// When a pool last handed an item out.
struct TextureHandOutStamp {
  /// Pool frame of the last hand-out; zero when the pool never handed the item out.
  uint64_t frame = 0;
  /// Pool-wide sequence number of that hand-out, ordering hand-outs within one frame; zero when
  /// the pool never handed the item out.
  uint64_t order = 0;
};

/**
 * The free items of one pool bucket, handed out in the order the most recent frame took them,
 * whatever order they came back in, then those of earlier frames by recency.
 *
 * A frame that repeats the previous frame's acquisitions therefore gets back the same item for
 * each one, which keeps caches keyed on item identity valid across unchanged frames. Items never
 * handed out, and items whose stamp was dropped, carry a zero stamp: they go out after every item
 * handed out, including those of the pool's first frame, in the order they were queued.
 *
 * \ref push, \ref takeNext and \ref takeLast each cost O(log n) and move the item a constant
 * number of times, for in-order and out-of-order returns alike, so a bucket of many small textures
 * costs no more per return than a bucket of a few.
 *
 * @tparam Item Move-only or copyable item type, such as a texture handle.
 */
template <typename Item>
class TextureHandOutQueue {
public:
  /// True when no item is queued.
  bool empty() const { return items_.empty(); }

  /// Number of queued items.
  size_t size() const { return items_.size(); }

  /// Queues \p item under \p stamp.
  /// @param item Item to queue; moved from.
  /// @param stamp When the pool last handed \p item out.
  void push(Item item, TextureHandOutStamp stamp) { items_.emplace(stamp, std::move(item)); }

  /// Removes and returns the item to hand out next. Requires a non-empty queue.
  Item takeNext() { return take(items_.begin()); }

  /// Removes and returns the item the queue would hand out last, the one to evict first. Requires
  /// a non-empty queue.
  Item takeLast() { return take(std::prev(items_.end())); }

  /// Removes every item, passing each to \p consume.
  /// @param consume Called with each item as an rvalue.
  template <typename Consume>
  void drain(Consume&& consume) {
    for (auto& [unusedStamp, item] : items_) {
      (void)unusedStamp;
      consume(std::move(item));
    }
    items_.clear();
  }

private:
  /// Orders stamps from the next hand-out to the last. A zero order marks an item never handed
  /// out, which follows every handed-out item of the same frame.
  struct HandsOutFirst {
    bool operator()(const TextureHandOutStamp& lhs, const TextureHandOutStamp& rhs) const {
      if (lhs.frame != rhs.frame) {
        return lhs.frame > rhs.frame;
      }
      return lhs.order != 0 && (rhs.order == 0 || lhs.order < rhs.order);
    }
  };

  using Items = std::multimap<TextureHandOutStamp, Item, HandsOutFirst>;

  Item take(typename Items::iterator position) {
    Item item = std::move(position->second);
    items_.erase(position);
    return item;
  }

  Items items_;
};

}  // namespace donner::svg
