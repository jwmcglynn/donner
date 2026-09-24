/// @file
/// The sticky loss condition's releases: work blocked on the device behind a wait only a healthy
/// root would satisfy is released by the declaration that sets the condition.

#include "donner/gpu/DeviceLost.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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
