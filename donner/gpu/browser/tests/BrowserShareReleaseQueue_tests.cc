#include "donner/gpu/browser/BrowserShareReleaseQueue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

}  // namespace
}  // namespace donner::gpu::browser
