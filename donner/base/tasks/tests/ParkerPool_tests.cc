#include "donner/base/tasks/ParkerPool.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "donner/base/tasks/tests/SchedulerTestUtils.h"

namespace donner::tasks {
namespace {

using testing::Eq;
using testing::Ne;

TEST(ParkerPoolTest, AcquireHandsOutDistinctSlotsAtStableAddresses) {
  constexpr std::size_t kSlots = 100;  // More than one block.
  ParkerPool pool;
  std::vector<ParkerPool::Slot> slots;
  std::set<const Parker<>*> parkers;
  for (std::size_t i = 0; i < kSlots; ++i) {
    slots.push_back(pool.acquire());
    parkers.insert(slots.back().parker);
  }
  EXPECT_THAT(parkers.size(), Eq(kSlots)) << "every slot has its own parker";
  EXPECT_THAT(pool.slotsInUse(), Eq(kSlots));
  EXPECT_THAT(pool.slotCount(), Eq(128u)) << "slots come in blocks of 32";
  for (const ParkerPool::Slot& slot : slots) {
    pool.release(slot.index);
  }
  EXPECT_THAT(pool.slotsInUse(), Eq(0u));

  std::set<const Parker<>*> reacquired;
  for (std::size_t i = 0; i < kSlots; ++i) {
    reacquired.insert(pool.acquire().parker);
  }
  EXPECT_THAT(reacquired, Eq(parkers)) << "returned slots are reused at the same addresses";
  EXPECT_THAT(pool.slotCount(), Eq(128u));
  for (const ParkerPool::Slot& slot : slots) {
    pool.release(slot.index);
  }
}

TEST(ParkerPoolTest, ConcurrentRegistrationsGetDistinctParkers) {
  constexpr std::size_t kThreads = 8;
  auto pool = std::make_shared<ParkerPool>();
  const std::uint64_t generation = ParkerPool::NextGeneration();
  std::vector<const Parker<>*> parkers(kThreads, nullptr);
  Gate start;
  Completion registered;
  Gate exit;
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      start.arriveAndWait();
      parkers[i] = ParkerPool::ParkerForThread(generation, pool);
      registered.arrive();
      exit.arriveAndWait();
    });
  }
  start.release();
  EXPECT_TRUE(registered.waitFor(static_cast<int>(kThreads)));
  EXPECT_THAT(pool->slotsInUse(), Eq(kThreads)) << "each live thread holds one slot";
  exit.release();
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_THAT(std::set<const Parker<>*>(parkers.begin(), parkers.end()).size(), Eq(kThreads));
  EXPECT_THAT(pool->slotsInUse(), Eq(0u)) << "each thread's exit returned its slot";
}

TEST(ParkerPoolTest, AThreadKeepsOneParkerPerScheduler) {
  auto first = std::make_shared<ParkerPool>();
  auto second = std::make_shared<ParkerPool>();
  const std::uint64_t firstGeneration = ParkerPool::NextGeneration();
  const std::uint64_t secondGeneration = ParkerPool::NextGeneration();
  EXPECT_THAT(firstGeneration, Ne(secondGeneration));
  std::thread thread([&] {
    Parker<>* a = ParkerPool::ParkerForThread(firstGeneration, first);
    EXPECT_THAT(ParkerPool::ParkerForThread(firstGeneration, first), Eq(a))
        << "a second lookup for the same scheduler finds the same parker";
    Parker<>* b = ParkerPool::ParkerForThread(secondGeneration, second);
    EXPECT_THAT(b, Ne(a));
    EXPECT_THAT(first->slotsInUse(), Eq(1u));
    EXPECT_THAT(second->slotsInUse(), Eq(1u));
  });
  thread.join();
  EXPECT_THAT(first->slotsInUse(), Eq(0u));
  EXPECT_THAT(second->slotsInUse(), Eq(0u));
}

TEST(ParkerPoolTest, ThreadsThatExitReturnTheirSlotsForReuse) {
  auto pool = std::make_shared<ParkerPool>();
  const std::uint64_t generation = ParkerPool::NextGeneration();
  std::set<const Parker<>*> parkers;
  for (int i = 0; i < 50; ++i) {
    const Parker<>* parker = nullptr;
    std::thread thread([&] { parker = ParkerPool::ParkerForThread(generation, pool); });
    thread.join();
    parkers.insert(parker);
    EXPECT_THAT(pool->slotsInUse(), Eq(0u)) << "thread " << i;
  }
  EXPECT_THAT(parkers.size(), Eq(1u)) << "one slot serves threads that come and go";
}

TEST(ParkerPoolTest, ARegistrationDropsPoolsThatWereClosed) {
  const std::size_t poolsBefore = ParkerPool::LiveCountForTesting();
  std::size_t poolsWhileHeld = 0;
  std::size_t poolsAfterSweep = 0;
  std::thread thread([&] {
    auto closedPool = std::make_shared<ParkerPool>();
    (void)ParkerPool::ParkerForThread(ParkerPool::NextGeneration(), closedPool);
    closedPool->close();
    closedPool.reset();
    poolsWhileHeld = ParkerPool::LiveCountForTesting();

    auto openPool = std::make_shared<ParkerPool>();
    (void)ParkerPool::ParkerForThread(ParkerPool::NextGeneration(), openPool);
    openPool.reset();
    poolsAfterSweep = ParkerPool::LiveCountForTesting();
  });
  thread.join();
  EXPECT_THAT(poolsWhileHeld, Eq(poolsBefore + 1)) << "the thread's entry keeps the closed pool";
  EXPECT_THAT(poolsAfterSweep, Eq(poolsBefore + 1)) << "the next registration dropped it";
  EXPECT_THAT(ParkerPool::LiveCountForTesting(), Eq(poolsBefore));
}

TEST(ParkerPoolDeathTest, ReturningASlotNotInUseFails) {
  EXPECT_DEATH(
      {
        ParkerPool pool;
        const ParkerPool::Slot slot = pool.acquire();
        pool.release(slot.index);
        pool.release(slot.index);
      },
      "not in use");
}

TEST(ParkerPoolDeathTest, DestroyingAPoolWithASlotInUseFails) {
  EXPECT_DEATH(
      {
        ParkerPool pool;
        (void)pool.acquire();
      },
      "slot in use");
}

}  // namespace
}  // namespace donner::tasks
