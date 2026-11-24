#include "donner/base/tasks/ParallelFor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using testing::Eq;

class PublishCounter : public SchedulerTestHooks {
public:
  void at(SchedulerStep step, std::size_t) override {
    if (step == SchedulerStep::AfterPublishPush) {
      published.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::atomic<int> published{0};
};

TEST(ParallelForTest, BelowGrainRunsOneRangeInlineWithoutPublishing) {
  PublishCounter counter;
  auto scheduler = TaskSchedulerTestPeer::Make(TaskSchedulerOptions{.workerCount = 2}, &counter);
  const auto caller = std::this_thread::get_id();
  std::atomic<unsigned> calls{0};
  ParallelFor(*scheduler, 12, 19, 8, [&](std::size_t first, std::size_t last) {
    EXPECT_THAT(std::this_thread::get_id(), Eq(caller));
    EXPECT_THAT(first, Eq(12u));
    EXPECT_THAT(last, Eq(19u));
    ++calls;
  });
  EXPECT_THAT(calls.load(), Eq(1u));
  EXPECT_THAT(counter.published.load(), Eq(0));
  ParallelFor(*scheduler, 0, 15, 8, [](std::size_t, std::size_t) {});
  EXPECT_THAT(counter.published.load(), Eq(0));
  ParallelFor(*scheduler, 0, 16, 8,
              [](std::size_t first, std::size_t last) { EXPECT_THAT(last - first, Eq(8u)); });
  EXPECT_GT(counter.published.load(), 0);
}

TEST(ParallelForTest, ZeroWorkersRunOneRangeOnTheCallingThread) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
  const auto caller = std::this_thread::get_id();
  std::atomic<unsigned> calls{0};
  ParallelFor(scheduler, 5, 10005, 1, [&](std::size_t first, std::size_t last) {
    EXPECT_THAT(first, Eq(5u));
    EXPECT_THAT(last, Eq(10005u));
    EXPECT_THAT(std::this_thread::get_id(), Eq(caller));
    ++calls;
  });
  EXPECT_THAT(calls.load(), Eq(1u));
}

TEST(ParallelForTest, CoversEachIndexOnceAndJoinsBeforeReturning) {
  constexpr std::size_t kLength = 4099;
  constexpr std::size_t kEnd = std::numeric_limits<std::size_t>::max();
  constexpr std::size_t kBegin = kEnd - kLength;
  for (const std::size_t workers : {0u, 1u, 4u}) {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = workers});
    std::vector<std::atomic<unsigned>> visits(kLength);
    std::vector<std::size_t> results(kLength);
    std::atomic<bool> invalid{false};
    ParallelFor(scheduler, kBegin, kEnd, 17, [&](std::size_t first, std::size_t last) {
      if (first < kBegin || last <= first || last - first < 17) {
        invalid.store(true);
        return;
      }
      for (std::size_t i = first; i < last; ++i) {
        const std::size_t offset = i - kBegin;
        if (visits[offset].fetch_add(1) == 0) {
          results[offset] = 3 * offset + 1;
        }
      }
    });
    EXPECT_FALSE(invalid.load());
    for (std::size_t i = 0; i < kLength; ++i) {
      ASSERT_THAT(visits[i].load(), Eq(1u)) << "index " << i;
      ASSERT_THAT(results[i], Eq(3 * i + 1)) << "index " << i;
    }
  }
}

TEST(ParallelForTest, RecordPressureKeepsEveryUnsubmittedRange) {
  std::array<Gate, 2> busy;
  std::array<std::atomic<unsigned>, 37> sizes{};
  TaskScheduler scheduler(
      TaskSchedulerOptions{.workerCount = 3,
                           .maxTasksPerCreator = 1,
                           .idleSpinRounds = std::numeric_limits<std::size_t>::max()});
  TaskScope holders(scheduler);
  for (Gate& gate : busy) {
    holders.spawn([&gate] { gate.arriveAndWait(); });
  }
  for (Gate& gate : busy) {
    EXPECT_TRUE(gate.waitForArrival());
  }
  // The remaining worker queues the right half, filling its pool before its next split.
  std::atomic<unsigned> calls{0};
  ParallelFor(scheduler, 0, sizes.size(), 1, [&](std::size_t first, std::size_t last) {
    if (first >= sizes.size() || last <= first || last > sizes.size()) {
      ADD_FAILURE() << "invalid range " << first << ".." << last;
      return;
    }
    ++calls;
    sizes[first].store(static_cast<unsigned>(last - first));
  });
  for (Gate& gate : busy) {
    gate.release();
  }
  holders.wait();
  for (Gate& gate : busy) {
    EXPECT_FALSE(gate.heldPastHangGuard());
  }
  EXPECT_THAT(calls.load(), Eq(2u));
  EXPECT_THAT(sizes[0].load(), Eq(18u));
  EXPECT_THAT(sizes[18].load(), Eq(19u));
}

TEST(ParallelForTest, FullExternalPoolLeavesTheWholeRangeWithTheCaller) {
  Gate busy;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 2, .maxExternalTasks = 1});
  TaskScope holder(scheduler);
  holder.spawn([&] { busy.arriveAndWait(); });
  EXPECT_TRUE(busy.waitForArrival());
  const auto caller = std::this_thread::get_id();
  std::atomic<unsigned> calls{0};
  ParallelFor(scheduler, 19, 108, 1, [&](std::size_t first, std::size_t last) {
    EXPECT_THAT(std::this_thread::get_id(), Eq(caller));
    EXPECT_THAT(first, Eq(19u));
    EXPECT_THAT(last, Eq(108u));
    ++calls;
  });
  busy.release();
  holder.wait();
  EXPECT_FALSE(busy.heldPastHangGuard());
  EXPECT_THAT(calls.load(), Eq(1u));
}

TEST(ParallelForTest, UnstolenHalfStaysCoarseOnABusyPool) {
  Gate busy;
  TaskScheduler scheduler(TaskSchedulerOptions{
      .workerCount = 2, .idleSpinRounds = std::numeric_limits<std::size_t>::max()});
  TaskScope holder(scheduler);
  holder.spawn([&] { busy.arriveAndWait(); });
  EXPECT_TRUE(busy.waitForArrival());
  std::array<std::atomic<unsigned>, 16> sizes{};
  ParallelFor(scheduler, 0, sizes.size(), 1, [&](std::size_t first, std::size_t last) {
    if (first >= sizes.size() || last <= first || last > sizes.size()) {
      ADD_FAILURE() << "invalid range " << first << ".." << last;
      return;
    }
    sizes[first].store(static_cast<unsigned>(last - first));
  });
  busy.release();
  holder.wait();
  EXPECT_FALSE(busy.heldPastHangGuard());
  EXPECT_THAT(sizes[0].load(), Eq(8u));
  EXPECT_THAT(sizes[8].load(), Eq(8u));
}

TEST(ParallelForTest, EmptyRangeNeverInvokesTheBody) {
  for (const auto workers : {0u, 2u}) {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = workers});
    const auto failIfCalled = [](std::size_t, std::size_t) { ADD_FAILURE() << "empty callback"; };
    ParallelFor(scheduler, 7, 7, 0, failIfCalled);
    detail::ParallelForWithStrategy(detail::ParallelForStrategy::SharedCursor, scheduler, 7, 7, 0,
                                    failIfCalled);
  }
}

TEST(ParallelForDeathTest, ReversedBoundsFailBeforePartitioning) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0});
  const auto body = [](std::size_t, std::size_t) {};
  EXPECT_DEATH(ParallelFor(scheduler, 3, 2, 0, body), "ParallelFor requires begin <= end");
  EXPECT_DEATH(detail::ParallelForWithStrategy(detail::ParallelForStrategy::SharedCursor, scheduler,
                                               3, 2, 0, body),
               "ParallelFor requires begin <= end");
}

TEST(ParallelForTest, ZeroGrainUsesOneBeforeApplyingTheChunkCap) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 2});
  for (const auto strategy :
       {detail::ParallelForStrategy::Adaptive, detail::ParallelForStrategy::SharedCursor}) {
    std::array<std::atomic<unsigned>, 2> sizes{};
    detail::ParallelForWithStrategy(strategy, scheduler, 0, 2, 0,
                                    [&](std::size_t first, std::size_t last) {
                                      if (first >= sizes.size()) {
                                        ADD_FAILURE() << "invalid first index " << first;
                                        return;
                                      }
                                      sizes[first].store(static_cast<unsigned>(last - first));
                                    });
    EXPECT_THAT(sizes[0].load(), Eq(1u));
    EXPECT_THAT(sizes[1].load(), Eq(1u));
  }
  EXPECT_THAT(detail::ParallelForGrain(0, 0), Eq(1u));
  EXPECT_THAT(detail::ParallelForGrain(4097, 0), Eq(2u));
}

TEST(ParallelForTest, SharedCursorCoversTheTailNearTheLargestIndex) {
  constexpr auto kEnd = std::numeric_limits<std::size_t>::max();
  constexpr std::size_t kLength = 37;
  constexpr auto kBegin = kEnd - kLength;
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 4});
  std::array<std::atomic<unsigned>, kLength> visits{};
  detail::ParallelForWithStrategy(detail::ParallelForStrategy::SharedCursor, scheduler, kBegin,
                                  kEnd, 5, [&](std::size_t first, std::size_t last) {
                                    if (first < kBegin || last <= first || last - first > 5) {
                                      ADD_FAILURE()
                                          << "invalid cursor range " << first << ".." << last;
                                      return;
                                    }
                                    for (std::size_t i = first; i < last; ++i) {
                                      ++visits[i - kBegin];
                                    }
                                  });
  for (const auto& seen : visits) {
    EXPECT_THAT(seen.load(), Eq(1u));
  }
}

TEST(ParallelForTest, SeededLoopsCoverTheSameIndicesOnTheCaller) {
  for (const auto strategy :
       {detail::ParallelForStrategy::Adaptive, detail::ParallelForStrategy::SharedCursor}) {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 0, .seed = 13});
    const auto caller = std::this_thread::get_id();
    std::array<unsigned, 32> visits{};
    unsigned calls = 0;
    detail::ParallelForWithStrategy(
        strategy, scheduler, 0, visits.size(), 2, [&](std::size_t first, std::size_t last) {
          EXPECT_THAT(std::this_thread::get_id(), Eq(caller));
          ++calls;
          if (first >= last || last > visits.size()) {
            ADD_FAILURE() << "invalid replay range " << first << ".." << last;
            return;
          }
          for (std::size_t i = first; i < last; ++i) {
            ++visits[i];
          }
        });
    EXPECT_GT(calls, 1u);
    EXPECT_THAT(visits, testing::Each(1u));
  }
}

TEST(ParallelForTest, SeededAdaptiveRangeTraceIsIndependentOfRequestedWorkers) {
  const auto trace = [](std::uint64_t seed, std::size_t workers) {
    TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = workers, .seed = seed});
    std::vector<std::size_t> starts;
    ParallelFor(scheduler, 0, 64, 1,
                [&](std::size_t first, std::size_t) { starts.push_back(first); });
    return starts;
  };
  const auto original = trace(0, 0);
  EXPECT_GT(original.size(), 2u);
  EXPECT_THAT(trace(0, 4), Eq(original));
  bool sawDifferentOrder = false;
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    sawDifferentOrder |= trace(seed, 4) != original;
  }
  EXPECT_TRUE(sawDifferentOrder);
}

class ContestedClaimHooks : public SchedulerTestHooks {
public:
  void at(SchedulerStep step, std::size_t worker) override {
    if (step == SchedulerStep::ParallelForBeforeClaim) {
      const auto arrival = claims.fetch_add(1, std::memory_order_relaxed);
      if (arrival == 0) {
        firstWorker = worker;
        sawPeer = peer.wait();
      } else if (arrival == 1) {
        secondWorker = worker;
        peer.set();
      }
    } else if (step == SchedulerStep::ParallelForAfterClaim) {
      completedClaims.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::atomic<unsigned> claims{0};
  std::atomic<unsigned> completedClaims{0};
  std::size_t firstWorker = 0;
  std::size_t secondWorker = 0;
  Event peer;
  bool sawPeer = false;
};

TEST(ParallelForTest, CursorClaimsAreDisjointWhenTwoWorkersReachTheCursorTogether) {
  ContestedClaimHooks hooks;
  auto scheduler = TaskSchedulerTestPeer::Make(
      TaskSchedulerOptions{.workerCount = 2,
                           .idleSpinRounds = std::numeric_limits<std::size_t>::max()},
      &hooks);
  std::array<std::atomic<unsigned>, 16> visits{};
  detail::ParallelForWithStrategy(detail::ParallelForStrategy::SharedCursor, *scheduler, 0,
                                  visits.size(), 2, [&](std::size_t first, std::size_t last) {
                                    if (first >= last || last > visits.size()) {
                                      ADD_FAILURE() << "invalid claim " << first << ".." << last;
                                      return;
                                    }
                                    for (std::size_t i = first; i < last; ++i) {
                                      ++visits[i];
                                    }
                                  });
  EXPECT_TRUE(hooks.sawPeer);
  EXPECT_NE(hooks.firstWorker, hooks.secondWorker);
  EXPECT_THAT(hooks.claims.load(), Eq(10u));  // Eight chunks and two exhausted runners.
  EXPECT_THAT(hooks.completedClaims.load(), Eq(hooks.claims.load()));
  for (const auto& seen : visits) {
    EXPECT_THAT(seen.load(), Eq(1u));
  }
}

TEST(ParallelForTest, EffectiveGrainBoundsLeafCountWithoutOverflow) {
  constexpr std::size_t kMaximum = std::numeric_limits<std::size_t>::max();
  EXPECT_THAT(detail::ParallelForGrain(4096, 1), Eq(1u));
  EXPECT_THAT(detail::ParallelForGrain(4097, 1), Eq(2u));
  EXPECT_THAT(detail::ParallelForGrain(kMaximum, 1), Eq(kMaximum / 4096 + 1));
  EXPECT_THAT(detail::ParallelForGrain(kMaximum, kMaximum), Eq(kMaximum));
}

TEST(ParallelForTest, StolenRangeSplitsAgainWhileTheOwnerRunsTheFirstHalf) {
  TaskScheduler scheduler(TaskSchedulerOptions{
      .workerCount = 2, .idleSpinRounds = std::numeric_limits<std::size_t>::max()});
  Event thiefRan;
  bool sawThief = false;
  std::array<std::atomic<unsigned>, 16> sizes{};
  ParallelFor(scheduler, 0, 16, 1, [&](std::size_t first, std::size_t last) {
    if (first >= sizes.size() || last <= first || last > sizes.size()) {
      ADD_FAILURE() << "invalid range " << first << ".." << last;
      return;
    }
    sizes[first].store(static_cast<unsigned>(last - first));
    if (first == 0) {
      sawThief = thiefRan.wait();
    } else {
      thiefRan.set();
    }
  });
  EXPECT_TRUE(sawThief);
  EXPECT_THAT(sizes[0].load(), Eq(8u));
  EXPECT_THAT(sizes[8].load(), Eq(4u));
}

// Hold each left half until a thief enters the right half. This drives the public loop down
// to its minimum leaf size with just two workers, independently of their relative speeds.
TEST(ParallelForTest, RepeatedStealsHonorTheEffectiveGrain) {
  constexpr std::size_t kLength = 8192;
  std::array<Event, std::bit_width(kLength)> arrived;
  TaskScheduler scheduler(TaskSchedulerOptions{
      .workerCount = 2, .idleSpinRounds = std::numeric_limits<std::size_t>::max()});
  std::atomic<unsigned> calls{0};
  ParallelFor(scheduler, 0, kLength, 0, [&](std::size_t first, std::size_t last) {
    if (first >= last || last > kLength || !std::has_single_bit(kLength - first)) {
      ADD_FAILURE() << "unexpected interval " << first << ".." << last;
      return;
    }
    ++calls;
    arrived[std::countr_zero(kLength - first)].set();
    EXPECT_GE(last - first, 2u);
    if (last != kLength) {
      EXPECT_TRUE(arrived[std::countr_zero(kLength - last)].wait());
    }
  });
  // Twelve halvings take 8192 down to 2; the last split produces two leaves.
  EXPECT_THAT(calls.load(), Eq(13u));
}

TEST(ParallelForTest, WorkerWithFullDequeKeepsTheRangeAndPreservesOuterWork) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 1, .dequeCapacity = 2});
  std::atomic<unsigned> outerRuns{0};
  std::size_t covered = 0;
  TaskScope outer(scheduler);
  outer.spawn([&] {
    outer.spawn([&] { ++outerRuns; });
    outer.spawn([&] { ++outerRuns; });
    ParallelFor(scheduler, 3, 101, 1,
                [&](std::size_t first, std::size_t last) { covered += last - first; });
    EXPECT_THAT(covered, Eq(98u));
    EXPECT_THAT(outerRuns.load(), Eq(0u));
  });
  outer.wait();
  EXPECT_THAT(outerRuns.load(), Eq(2u));
}

class DelayRootUntilJoin : public SchedulerTestHooks {
public:
  void at(SchedulerStep step, std::size_t) override {
    if (step == SchedulerStep::ScopeWaiterBeforePrepareWait) {
      waiterEntered.store(true, std::memory_order_relaxed);
      worker.release();
    }
  }
  Gate worker;
  std::atomic<bool> waiterEntered{false};
};

TEST(ParallelForTest, DelayedRootBorrowsTheBodyUntilTheLoopJoins) {
  DelayRootUntilJoin hooks;
  std::size_t visited = 0;
  auto scheduler = TaskSchedulerTestPeer::Make(TaskSchedulerOptions{.workerCount = 1}, &hooks);
  TaskScope holder(*scheduler);
  holder.spawn([&] { hooks.worker.arriveAndWait(); });
  EXPECT_TRUE(hooks.worker.waitForArrival());
  ParallelFor(*scheduler, 0, 16, 1,
              [&](std::size_t first, std::size_t last) { visited += last - first; });
  EXPECT_TRUE(hooks.waiterEntered.load(std::memory_order_relaxed));
  EXPECT_THAT(visited, Eq(16u));
  hooks.worker.release();
  holder.wait();
  EXPECT_FALSE(hooks.worker.heldPastHangGuard());
}

struct ImmovableBody {
  explicit ImmovableBody(std::atomic<std::size_t>& total) : total(total) {}
  ImmovableBody(const ImmovableBody&) = delete;
  void operator()(std::size_t, std::size_t) = delete;
  void operator()(std::size_t&& first, std::size_t&& last) const { total.fetch_add(last - first); }
  std::atomic<std::size_t>& total;
};

TEST(ParallelForTest, SharesOneConstInvocableBody) {
  TaskScheduler scheduler(TaskSchedulerOptions{.workerCount = 4});
  std::atomic<std::size_t> total{0};
  ImmovableBody body(total);
  ParallelFor(scheduler, 0, 37, 1, body);
  EXPECT_THAT(total.load(), Eq(37u));
}

}  // namespace
}  // namespace donner::tasks
