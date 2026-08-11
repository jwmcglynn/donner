#include "donner/base/AsyncifySuspendProbe.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <thread>

namespace donner {
namespace {

using ::testing::Ge;

constexpr std::size_t kTileYield = static_cast<std::size_t>(SuspendKind::TileYield);
constexpr std::size_t kDeviceWait = static_cast<std::size_t>(SuspendKind::DeviceWait);

/// Burn at least @p ms of monotonic time without sleeping, so the assertions
/// hold on a loaded CI machine where a sleep can be preempted arbitrarily but
/// never returns early.
void SpinFor(double ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    // Spin.
  }
}

class AsyncifySuspendProbeTest : public ::testing::Test {
protected:
  void SetUp() override { BeginSuspendFrame(); }
  void TearDown() override { (void)EndSuspendFrame(); }
};

TEST_F(AsyncifySuspendProbeTest, EmptyFrameAttributesNothing) {
  const FrameSuspendTotals totals = EndSuspendFrame();
  EXPECT_EQ(totals.count, 0u);
  EXPECT_EQ(totals.totalMs, 0.0);
  EXPECT_EQ(totals.longestMs, 0.0);
}

TEST_F(AsyncifySuspendProbeTest, ScopeAttributesWallTimeToItsKind) {
  {
    const ScopedSuspendPoint suspend(SuspendKind::TileYield);
    SpinFor(2.0);
  }

  const FrameSuspendTotals totals = EndSuspendFrame();
  EXPECT_EQ(totals.count, 1u);
  EXPECT_EQ(totals.countByKind[kTileYield], 1u);
  EXPECT_EQ(totals.countByKind[kDeviceWait], 0u);
  EXPECT_THAT(totals.totalMs, Ge(2.0));
  EXPECT_THAT(totals.msByKind[kTileYield], Ge(2.0));
  EXPECT_EQ(totals.msByKind[kDeviceWait], 0.0);
  EXPECT_THAT(totals.longestMs, Ge(2.0));
}

TEST_F(AsyncifySuspendProbeTest, SeparateKindsAccumulateSeparately) {
  {
    const ScopedSuspendPoint suspend(SuspendKind::TileYield);
    SpinFor(1.0);
  }
  {
    const ScopedSuspendPoint suspend(SuspendKind::DeviceWait);
    SpinFor(3.0);
  }

  const FrameSuspendTotals totals = EndSuspendFrame();
  EXPECT_EQ(totals.count, 2u);
  EXPECT_EQ(totals.countByKind[kTileYield], 1u);
  EXPECT_EQ(totals.countByKind[kDeviceWait], 1u);
  EXPECT_THAT(totals.msByKind[kDeviceWait], Ge(totals.msByKind[kTileYield]));
  EXPECT_THAT(totals.longestMs, Ge(3.0));
  // `longestMs` is the largest single suspend, never the sum.
  EXPECT_LE(totals.longestMs, totals.totalMs);
}

// A readback wait that internally polls the device must charge the frame once
// for the interval, not once per nesting level: the intervals overlap, and
// double-charging would let attributed suspend time exceed the frame's own
// wall time.
TEST_F(AsyncifySuspendProbeTest, NestedScopesChargeTheOuterIntervalOnce) {
  {
    const ScopedSuspendPoint outer(SuspendKind::GpuReadback);
    SpinFor(1.0);
    {
      const ScopedSuspendPoint inner(SuspendKind::DeviceWait);
      SpinFor(1.0);
    }
    SpinFor(1.0);
  }

  const FrameSuspendTotals totals = EndSuspendFrame();
  EXPECT_EQ(totals.count, 1u);
  EXPECT_EQ(totals.countByKind[static_cast<std::size_t>(SuspendKind::GpuReadback)], 1u);
  EXPECT_EQ(totals.countByKind[kDeviceWait], 0u);
  EXPECT_THAT(totals.totalMs, Ge(3.0));
}

TEST_F(AsyncifySuspendProbeTest, BeginFrameDiscardsThePreviousWindow) {
  {
    const ScopedSuspendPoint suspend(SuspendKind::TileYield);
    SpinFor(1.0);
  }
  ASSERT_EQ(PeekSuspendFrame().count, 1u);

  BeginSuspendFrame();
  EXPECT_EQ(PeekSuspendFrame().count, 0u);
  EXPECT_EQ(PeekSuspendFrame().totalMs, 0.0);
}

TEST_F(AsyncifySuspendProbeTest, LifetimeTotalsSurviveFrameBoundaries) {
  const FrameSuspendTotals before = LifetimeSuspendTotals();
  {
    const ScopedSuspendPoint suspend(SuspendKind::TileYield);
    SpinFor(1.0);
  }
  BeginSuspendFrame();
  {
    const ScopedSuspendPoint suspend(SuspendKind::TileYield);
    SpinFor(1.0);
  }

  const FrameSuspendTotals after = LifetimeSuspendTotals();
  EXPECT_EQ(after.count, before.count + 2u);
  EXPECT_THAT(after.totalMs, Ge(before.totalMs + 2.0));
  EXPECT_EQ(PeekSuspendFrame().count, 1u) << "the frame window still holds only the second scope";
}

// Counters are thread-local: the pooled sample-thumbnail renderer and the app
// thread must not contaminate each other's per-frame attribution.
TEST_F(AsyncifySuspendProbeTest, CountersAreThreadLocal) {
  {
    const ScopedSuspendPoint suspend(SuspendKind::TileYield);
    SpinFor(1.0);
  }

  FrameSuspendTotals otherThreadTotals;
  std::thread other([&] {
    BeginSuspendFrame();
    {
      const ScopedSuspendPoint suspend(SuspendKind::DeviceWait);
      SpinFor(1.0);
    }
    otherThreadTotals = EndSuspendFrame();
  });
  other.join();

  EXPECT_EQ(otherThreadTotals.count, 1u);
  EXPECT_EQ(otherThreadTotals.countByKind[kDeviceWait], 1u);
  EXPECT_EQ(otherThreadTotals.countByKind[kTileYield], 0u)
      << "the other thread must not see this thread's tile yield";

  const FrameSuspendTotals totals = EndSuspendFrame();
  EXPECT_EQ(totals.count, 1u);
  EXPECT_EQ(totals.countByKind[kTileYield], 1u);
  EXPECT_EQ(totals.countByKind[kDeviceWait], 0u);
}

}  // namespace
}  // namespace donner
