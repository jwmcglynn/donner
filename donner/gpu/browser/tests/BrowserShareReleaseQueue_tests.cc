#include "donner/gpu/browser/BrowserShareReleaseQueue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace donner::gpu::browser {
namespace {

using Release = BrowserShareReleaseQueue::Release;

/// The shares of \p releases, in order.
std::vector<BrowserTextureShareId> SharesOf(const std::vector<Release>& releases) {
  std::vector<BrowserTextureShareId> shares;
  for (const Release& release : releases) {
    shares.push_back(release.share);
  }
  return shares;
}

TEST(BrowserShareReleaseQueue, HandsTheOwnerWhatOtherThreadsPostedOldestFirst) {
  BrowserShareReleaseQueue queue;
  std::thread elsewhere([&] {
    EXPECT_TRUE(queue.post({.producer = 1, .share = 7}));
    EXPECT_TRUE(queue.post({.producer = 1, .share = 9}));
  });
  elsewhere.join();

  const std::vector<Release> taken = queue.takeAll();
  EXPECT_THAT(SharesOf(taken), testing::ElementsAre(7u, 9u));
  EXPECT_THAT(taken.front().producer, 1u);
  EXPECT_THAT(queue.takeAll(), testing::IsEmpty()) << "a release was handed out twice";
}

TEST(BrowserShareReleaseQueue, WakesTheOwnerOnlyForAReleaseItQueued) {
  BrowserShareReleaseQueue queue;
  int wakes = 0;
  EXPECT_TRUE(queue.post({.producer = 1, .share = 7}, [&] { ++wakes; }));
  EXPECT_THAT(wakes, 1);

  queue.close();
  EXPECT_FALSE(queue.post({.producer = 1, .share = 8}, [&] { ++wakes; }));
  EXPECT_THAT(wakes, 1) << "a closed queue woke an owner that has gone";
}

TEST(BrowserShareReleaseQueue, AQueueWhoseOwnerHasGoneDropsWhatWaitsAndRefusesMore) {
  BrowserShareReleaseQueue queue;
  EXPECT_TRUE(queue.post({.producer = 1, .share = 7}));
  queue.close();
  EXPECT_TRUE(queue.closed());
  EXPECT_THAT(queue.takeAll(), testing::IsEmpty());
  EXPECT_FALSE(queue.post({.producer = 1, .share = 9}));
  EXPECT_THAT(queue.takeAll(), testing::IsEmpty());
}

TEST(BrowserShareReleaseQueue, EachThreadHasAQueueOfItsOwn) {
  const std::shared_ptr<BrowserShareReleaseQueue> here = BrowserShareReleaseQueue::ForThisThread();
  ASSERT_THAT(here, testing::NotNull());
  EXPECT_THAT(BrowserShareReleaseQueue::ForThisThread(), here);
  std::shared_ptr<BrowserShareReleaseQueue> elsewhere;
  std::thread other([&] { elsewhere = BrowserShareReleaseQueue::ForThisThread(); });
  other.join();
  ASSERT_THAT(elsewhere, testing::NotNull());
  EXPECT_THAT(elsewhere, testing::Ne(here));
  EXPECT_TRUE(elsewhere->closed()) << "a thread that exited left its queue open";

  EXPECT_TRUE(here->post({.producer = 1, .share = 7}));
  std::vector<BrowserTextureShareId> ran;
  BrowserShareReleaseQueue::DrainThisThread(
      [&](const Release& release) { ran.push_back(release.share); });
  EXPECT_THAT(ran, testing::ElementsAre(7u));
}

TEST(BrowserShareReleaseQueue, AnObjectTheThreadDestroysAfterItsQueueFindsNoQueue) {
  // 0: never ran; 1: found no queue and ran nothing; 2: reached a queue that had gone.
  std::atomic<int> outcome{0};
  std::thread worker([&outcome] {
    struct DestroyedAfterTheQueue {
      std::atomic<int>* outcome;
      ~DestroyedAfterTheQueue() {
        int ran = 0;
        BrowserShareReleaseQueue::DrainThisThread([&](const Release&) { ++ran; });
        const bool noQueue = BrowserShareReleaseQueue::ForThisThread() == nullptr;
        outcome->store(ran == 0 && noQueue ? 1 : 2);
      }
    };
    // Made before the thread's queue, so the thread destroys it after the queue, the way a
    // thread-exit-scoped owner of this thread's browser devices would be.
    thread_local DestroyedAfterTheQueue destroyedLast{&outcome};
    (void)destroyedLast;
    ASSERT_TRUE(BrowserShareReleaseQueue::ForThisThread()->post({.producer = 1, .share = 7}));
  });
  worker.join();
  EXPECT_THAT(outcome.load(), 1);
}

}  // namespace
}  // namespace donner::gpu::browser
