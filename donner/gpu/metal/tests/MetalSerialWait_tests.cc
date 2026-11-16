/// @file
/// How a Metal serial wait spends its time: it sleeps until the device's completion state changes,
/// so it returns as soon as the work it waits for completes instead of on its next recheck.
///
/// Whether a change wakes the wait is checked on a \ref donner::gpu::metal::TestProgressClock that
/// never moves: the wait's budget then never runs out, so a wait that missed its wake never
/// returns, however long a loaded machine takes to deliver it.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/metal/tests/MetalWaitTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal {
namespace {

using testing::Eq;
using testing::Ge;
using testing::IsTrue;
using testing::Le;

/// How long a case holds its work before letting it complete: long enough that a wait that
/// rechecks on a timer looks many times meanwhile, and well inside the system's own timeout for
/// command buffers that make no progress.
constexpr std::chrono::milliseconds kHeldFor{200};

/// Budget of the waits on the stopped clock, which never runs out unless a case that ends early
/// moves the clock past it.
constexpr double kBudgetSeconds = 5.0;

/// How far a case that ends early moves its stopped clock: past \ref kBudgetSeconds, so a wait
/// still in progress returns.
constexpr std::chrono::hours kPastTheBudget{1};

/// Most looks a wait for one held command buffer takes: one when it starts, one when the command
/// buffer completes and one when its serial is published, plus one spurious wakeup.
constexpr uint64_t kMaxLooks = 4;

/// Lets held work complete through \p release on a thread of its own once \ref kHeldFor has
/// passed; \ref PacedSteps::lastStepStart is when. @param release What lets the work complete.
PacedSteps ReleaseAfterHold(std::function<void()> release) {
  return PacedSteps(1, kHeldFor, [release = std::move(release)](int) { release(); });
}

class MetalSerialWaitTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "Metal serial wait gate");
  }

  /// Submits one empty command buffer. @return Its serial.
  uint64_t submitEmptyWork() {
    CommandBuffer commands =
        GetResultOrFail(GetResultOrFail(device_->createCommandEncoder())->finish());
    return GetResultOrFail(device_->submit(std::move(commands)));
  }

  std::unique_ptr<MetalDevice> device_;
};

/// The same device on a \ref TestProgressClock the case never moves.
class MetalSerialWaitOnStoppedClockTest : public MetalSerialWaitTest {
protected:
  void SetUp() override {
    MetalSerialWaitTest::SetUp();
    if (testing::Test::IsSkipped() || testing::Test::HasFatalFailure()) {
      return;
    }
    ASSERT_THAT(device_->setProgressClockForTest(clock_), IsOk());
  }

  /// Waits for \p serial on a worker thread, with a budget that never runs out unless the case
  /// ends early. @param serial Serial to wait for.
  std::unique_ptr<WaitOnWorker<bool>> waitOnWorker(uint64_t serial) {
    return std::make_unique<WaitOnWorker<bool>>(
        [this, serial] { return device_->waitForSerial(serial, kBudgetSeconds); },
        [this] {
          device_->releasePausedCommandBuffersForTest(std::numeric_limits<uint64_t>::max());
          device_->releaseHeldCompletionForTest();
          clock_->advance(kPastTheBudget);
        });
  }

  /// How the wait stood once it had judged its budget at the clock's time.
  /// @param worker The worker that waits.
  WaitStanding judged(const WaitOnWorker<bool>& worker) {
    return AwaitJudgedAtNow(*device_, *clock_, [&] { return worker.returned(); });
  }

  std::shared_ptr<TestProgressClock> clock_ = std::make_shared<TestProgressClock>();
};

/// A wait for held work looks at the completion state only when it changes. A wait that rechecks
/// every millisecond instead looks about once per millisecond the work is held, and on macOS each
/// of those sleeps runs long, which puts a floor of over a millisecond under every wait on the
/// GPU however soon its work completes.
TEST_F(MetalSerialWaitTest, AWaitForHeldWorkLooksOnlyWhenCompletionStateChanges) {
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const uint64_t serial = submitEmptyWork();

  const uint64_t looksBefore = device_->serialWaitLooksForTest();
  PacedSteps releaser =
      ReleaseAfterHold([this] { device_->releasePausedCommandBuffersForTest(1); });
  const bool completed = device_->waitForSerial(serial, /*timeoutSeconds=*/5.0);
  const auto returnedAt = std::chrono::steady_clock::now();
  releaser.join();
  const uint64_t looks = device_->serialWaitLooksForTest() - looksBefore;

  EXPECT_THAT(completed, IsTrue());
  EXPECT_THAT(MillisecondsBetween(releaser.lastStepStart(), returnedAt), Ge(0))
      << "the wait returned before its work was released";
  EXPECT_THAT(looks, Le(kMaxLooks)) << "the wait looked at the completion state " << looks
                                    << " times while its work was held for " << kHeldFor.count()
                                    << " ms, so it rechecks on a timer instead of sleeping until "
                                       "the work completes";
}

/// A command buffer completing wakes the wait. Its clock never moves, so its budget never runs out:
/// only that wake can end it.
TEST_F(MetalSerialWaitOnStoppedClockTest, ACompletedCommandBufferWakesTheWait) {
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const uint64_t serial = submitEmptyWork();

  const auto waited = waitOnWorker(serial);
  ASSERT_THAT(judged(*waited), Eq(WaitStanding::StillWaiting))
      << "the wait returned before its work was released";
  device_->releasePausedCommandBuffersForTest(1);
  ASSERT_THAT(waited->awaitReturn(), IsTrue())
      << "the command buffer completed but the wait never woke";

  EXPECT_THAT(waited->result(), IsTrue());
}

/// Publishing the serial wakes the wait on its own. The command buffer has completed, which also
/// wakes a waiter, but its serial is held back, so publishing it later is the only wake the wait
/// gets, and the clock never moves, so a wait that missed it never returns.
TEST_F(MetalSerialWaitOnStoppedClockTest, PublishingTheSerialWakesTheWait) {
  device_->holdNextCompletionForTest();
  const uint64_t serial = submitEmptyWork();
  ASSERT_THAT(device_->waitForCompletionHandlersForTest(1, /*timeoutSeconds=*/5.0), IsTrue())
      << "the command buffer's completion handler never ran";

  const auto waited = waitOnWorker(serial);
  ASSERT_THAT(judged(*waited), Eq(WaitStanding::StillWaiting))
      << "the wait returned before its serial was published";
  device_->releaseHeldCompletionForTest();
  ASSERT_THAT(waited->awaitReturn(), IsTrue()) << "publishing the serial did not wake the wait";

  EXPECT_THAT(waited->result(), IsTrue());
}

}  // namespace
}  // namespace donner::gpu::metal
