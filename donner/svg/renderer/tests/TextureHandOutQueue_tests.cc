#include "donner/svg/renderer/TextureHandOutQueue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace donner::svg {
namespace {

using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::Eq;
using testing::Le;

/// Moves performed on \ref CountedItem values that share one tally.
struct MoveTally {
  size_t moves = 0;  //!< Move constructions and move assignments.
};

/// A move-only item that counts every time it is moved, standing in for a texture handle.
class CountedItem {
public:
  /// @param id Identity reported by \ref id. @param tally Tally every move of this item adds to.
  CountedItem(int id, MoveTally* tally) : id_(id), tally_(tally) {}

  CountedItem(const CountedItem&) = delete;
  CountedItem& operator=(const CountedItem&) = delete;

  /// Moves \p other, counting the move. @param other Item to move from.
  CountedItem(CountedItem&& other) noexcept : id_(other.id_), tally_(other.tally_) {
    ++tally_->moves;
  }

  /// Moves \p other, counting the move. @param other Item to move from.
  CountedItem& operator=(CountedItem&& other) noexcept {
    id_ = other.id_;
    tally_ = other.tally_;
    ++tally_->moves;
    return *this;
  }

  /// Identity given at construction.
  int id() const { return id_; }

private:
  int id_;
  MoveTally* tally_;
};

/// Ids of every item of \p queue, in hand-out order, emptying it.
std::vector<int> TakeAllInHandOutOrder(TextureHandOutQueue<CountedItem>& queue) {
  std::vector<int> ids;
  while (!queue.empty()) {
    ids.push_back(queue.takeNext().id());
  }
  return ids;
}

TEST(TextureHandOutQueue, HandsOutTheLatestFrameInItsAcquisitionOrderThenOlderFrames) {
  MoveTally tally;
  TextureHandOutQueue<CountedItem> queue;
  queue.push(CountedItem(1, &tally), TextureHandOutStamp{});
  queue.push(CountedItem(2, &tally), TextureHandOutStamp{.frame = 7, .order = 5});
  queue.push(CountedItem(3, &tally), TextureHandOutStamp{.frame = 8, .order = 9});
  queue.push(CountedItem(4, &tally), TextureHandOutStamp{.frame = 8, .order = 7});
  queue.push(CountedItem(5, &tally), TextureHandOutStamp{});
  queue.push(CountedItem(6, &tally), TextureHandOutStamp{.frame = 8, .order = 8});

  EXPECT_THAT(TakeAllInHandOutOrder(queue), ElementsAre(4, 6, 3, 2, 1, 5));
}

TEST(TextureHandOutQueue, TakeLastRemovesWhatWouldGoOutLast) {
  MoveTally tally;
  TextureHandOutQueue<CountedItem> queue;
  queue.push(CountedItem(1, &tally), TextureHandOutStamp{.frame = 3, .order = 10});
  queue.push(CountedItem(2, &tally), TextureHandOutStamp{.frame = 2, .order = 4});
  queue.push(CountedItem(3, &tally), TextureHandOutStamp{.frame = 3, .order = 11});

  EXPECT_THAT(queue.takeLast().id(), Eq(2));
  EXPECT_THAT(queue.takeLast().id(), Eq(3));
  EXPECT_THAT(TakeAllInHandOutOrder(queue), ElementsAre(1));
}

/// A bucket of many same-size textures, returned at a frame boundary in the order they were taken
/// and in the reverse order, with older-frame leftovers still queued. Every push, take and eviction
/// moves an item a constant number of times, so the cost per return does not grow with the bucket.
TEST(TextureHandOutQueue, LargeBucketMovesEachItemAConstantNumberOfTimes) {
  constexpr int kLeftovers = 2000;
  constexpr int kReturns = 20000;
  for (const bool reversed : {false, true}) {
    SCOPED_TRACE(reversed ? "returned in reverse acquisition order" : "returned in order");
    MoveTally tally;
    TextureHandOutQueue<CountedItem> queue;
    for (int i = 0; i < kLeftovers; ++i) {
      queue.push(CountedItem(-1 - i, &tally),
                 TextureHandOutStamp{.frame = 1, .order = static_cast<uint64_t>(i + 1)});
    }
    for (int i = 0; i < kReturns; ++i) {
      const int taken = reversed ? kReturns - 1 - i : i;
      queue.push(CountedItem(taken, &tally),
                 TextureHandOutStamp{.frame = 2, .order = static_cast<uint64_t>(10000 + taken)});
    }
    EXPECT_THAT(tally.moves, Le(size_t{kLeftovers + kReturns}));

    tally.moves = 0;
    std::vector<int> expected;
    std::vector<int> handedOut;
    for (int i = 0; i < kReturns; ++i) {
      expected.push_back(i);
      handedOut.push_back(queue.takeNext().id());
    }
    EXPECT_THAT(handedOut, ElementsAreArray(expected));
    for (int i = 0; i < kLeftovers; ++i) {
      EXPECT_THAT(queue.takeLast().id(), Eq(-kLeftovers + i)) << "eviction " << i;
    }
    EXPECT_THAT(tally.moves, Le(size_t{2} * (kReturns + kLeftovers)));
  }
}

}  // namespace
}  // namespace donner::svg
