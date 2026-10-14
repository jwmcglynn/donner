#include "donner/gpu/browser/BrowserBindGroupConstruction.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <numeric>
#include <vector>

namespace donner::gpu::browser {
namespace {

struct ConstructionProbe {
  int dispatches = 0;
  bool inOwner = false;
  int failAt = -1;
  BridgeStatus failure = BridgeStatus::Failed;
  std::vector<int> calls;

  BridgeStatus run(std::span<const int> entries) {
    calls.clear();
    calls.reserve(kMaxBindings + 2);
    const auto call = [&](int stage) {
      EXPECT_TRUE(inOwner);
      calls.push_back(stage);
      return stage == failAt ? failure : BridgeStatus::Success;
    };
    return ConstructBrowserBindGroup(
        entries, [&] { return call(0); }, [&](const int& entry) { return call(entry); },
        [&] { return call(100); },
        [&](auto&& operation) {
          EXPECT_FALSE(inOwner);
          ++dispatches;
          inOwner = true;
          operation();
          inOwner = false;
        });
  }
};

TEST(BrowserBindGroupConstruction, ZeroAndMaximumEntriesUseOneDispatchInOrder) {
  std::array<int, kMaxBindings> entries;
  std::iota(entries.begin(), entries.end(), 1);
  for (const std::size_t count : {std::size_t(0), std::size_t(kMaxBindings)}) {
    SCOPED_TRACE(count);
    ConstructionProbe probe;
    EXPECT_EQ(probe.run(std::span<const int>(entries).first(count)), BridgeStatus::Success);
    EXPECT_EQ(probe.dispatches, 1);
    std::vector<int> expected{0};
    expected.insert(expected.end(), entries.begin(), entries.begin() + count);
    expected.push_back(100);
    EXPECT_EQ(probe.calls, expected);
    EXPECT_FALSE(probe.inOwner);
  }
}

TEST(BrowserBindGroupConstruction, OversizedInputDoesNotDispatchOrTouchDescriptorState) {
  const std::array<int, kMaxBindings + 1> entries{};
  ConstructionProbe probe;
  EXPECT_EQ(probe.run(entries), BridgeStatus::Failed);
  EXPECT_EQ(probe.dispatches, 0);
  EXPECT_THAT(probe.calls, testing::IsEmpty());
  const std::array<int, 1> valid{1};
  EXPECT_EQ(probe.run(valid), BridgeStatus::Success);
  EXPECT_EQ(probe.dispatches, 1);
  EXPECT_THAT(probe.calls, testing::ElementsAre(0, 1, 100));
}

TEST(BrowserBindGroupConstruction, FirstFailureStopsTheSequenceAndNextConstructionRecovers) {
  const std::array<int, 3> entries{1, 2, 3};
  for (const auto failure :
       {BridgeStatus::UnknownObject, BridgeStatus::WrongObjectKind, BridgeStatus::NotOwner,
        BridgeStatus::DeviceLost, BridgeStatus::Failed}) {
    SCOPED_TRACE(failure);
    for (const int stage : {0, 2, 100}) {
      SCOPED_TRACE(stage);
      ConstructionProbe probe;
      probe.failAt = stage;
      probe.failure = failure;
      EXPECT_EQ(probe.run(entries), failure);
      EXPECT_EQ(probe.dispatches, 1);
      if (stage == 0) {
        EXPECT_THAT(probe.calls, testing::ElementsAre(0));
      } else if (stage == 2) {
        EXPECT_THAT(probe.calls, testing::ElementsAre(0, 1, 2));
      } else {
        EXPECT_THAT(probe.calls, testing::ElementsAre(0, 1, 2, 3, 100));
      }
      EXPECT_FALSE(probe.inOwner);
      probe.failAt = -1;
      EXPECT_EQ(probe.run(entries), BridgeStatus::Success);
      EXPECT_EQ(probe.dispatches, 2);
      EXPECT_THAT(probe.calls, testing::ElementsAre(0, 1, 2, 3, 100));
    }
  }
}

}  // namespace
}  // namespace donner::gpu::browser
