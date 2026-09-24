/// @file
/// The sticky loss condition's releases: work blocked on the device behind a wait only a healthy
/// root would satisfy is released by the declaration that sets the condition.

#include "donner/gpu/DeviceLost.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>

namespace donner::gpu {
namespace {

using testing::Eq;
using testing::IsFalse;
using testing::IsTrue;

/// Counts how often it was released.
class CountingRelease final : public DeviceLossRelease {
public:
  void releaseOnLoss() override { ++releases; }

  int releases = 0;  //!< Times \ref releaseOnLoss ran.
};

/// Records the attribution its condition carries when it is released.
class AttributionRecordingRelease final : public DeviceLossRelease {
public:
  /// @param state Condition this release is registered with.
  explicit AttributionRecordingRelease(const DeviceLostState& state) : state_(state) {}

  void releaseOnLoss() override {
    site = state_.timedOutSite.load(std::memory_order_acquire);
    elapsedMs = state_.timedOutElapsedMs.load(std::memory_order_relaxed);
  }

  DeviceLostWaitSite site = DeviceLostWaitSite::None;  //!< Site seen by the release.
  int elapsedMs = 0;                                   //!< Elapsed time seen by the release.

private:
  const DeviceLostState& state_;
};

TEST(DeviceLostStateTest, TheDeclarationRunsEveryRegisteredReleaseOnce) {
  DeviceLostState state;
  const auto first = std::make_shared<CountingRelease>();
  const auto second = std::make_shared<CountingRelease>();
  state.addLossRelease(first);
  state.addLossRelease(second);
  EXPECT_THAT(first->releases, Eq(0)) << "a release ran before any loss was declared";

  ASSERT_THAT(DeclareDeviceLost(state), IsTrue());
  EXPECT_THAT(first->releases, Eq(1));
  EXPECT_THAT(second->releases, Eq(1));

  EXPECT_THAT(DeclareDeviceLost(state), IsFalse());
  EXPECT_THAT(first->releases, Eq(1)) << "a later declaration of the same loss ran it again";
}

TEST(DeviceLostStateTest, ATimedOutWaitsDeclarationRunsTheReleasesToo) {
  DeviceLostState state;
  const auto release = std::make_shared<CountingRelease>();
  state.addLossRelease(release);

  ASSERT_THAT(DeclareDeviceLostAfterWaitTimeout(state, DeviceLostWaitSite::QueueIdle,
                                                std::chrono::milliseconds(5)),
              IsTrue());

  EXPECT_THAT(release->releases, Eq(1));
  EXPECT_THAT(state.timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
}

/// What a release lets run, such as a consumer's held work, can report the loss as soon as it
/// runs, so the condition already carries the wait that declared it when the release runs.
TEST(DeviceLostStateTest, AReleaseSeesTheWaitThatDeclaredTheLoss) {
  DeviceLostState state;
  const auto release = std::make_shared<AttributionRecordingRelease>(state);
  state.addLossRelease(release);

  ASSERT_THAT(DeclareDeviceLostAfterWaitTimeout(state, DeviceLostWaitSite::Present,
                                                std::chrono::milliseconds(250)),
              IsTrue());

  EXPECT_THAT(release->site, Eq(DeviceLostWaitSite::Present))
      << "the release ran before the loss carried its wait site";
  EXPECT_THAT(release->elapsedMs, Eq(250));
}

TEST(DeviceLostStateTest, AReleaseRegisteredAfterTheDeclarationRunsAtOnce) {
  DeviceLostState state;
  ASSERT_THAT(DeclareDeviceLost(state), IsTrue());

  const auto release = std::make_shared<CountingRelease>();
  state.addLossRelease(release);

  EXPECT_THAT(release->releases, Eq(1))
      << "no later declaration would run it, so waiting for one leaves it blocked";
}

TEST(DeviceLostStateTest, RegisteringOneReleaseTwiceRunsItOnce) {
  DeviceLostState state;
  const auto release = std::make_shared<CountingRelease>();
  state.addLossRelease(release);
  state.addLossRelease(release);

  ASSERT_THAT(DeclareDeviceLost(state), IsTrue());

  EXPECT_THAT(release->releases, Eq(1));
}

TEST(DeviceLostStateTest, AReleaseWhoseOwnerIsGoneIsNeitherKeptAliveNorRun) {
  DeviceLostState state;
  auto release = std::make_shared<CountingRelease>();
  const std::weak_ptr<CountingRelease> observed = release;
  state.addLossRelease(release);

  release.reset();
  EXPECT_THAT(observed.expired(), IsTrue()) << "registering a release kept its owner alive";

  EXPECT_THAT(DeclareDeviceLost(state), IsTrue());
}

}  // namespace
}  // namespace donner::gpu
