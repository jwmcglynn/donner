/// @file
/// How a Metal serial wait spends its time: it sleeps until the device's completion state changes,
/// so it returns as soon as the work it waits for completes instead of on its next recheck.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal {
namespace {

using testing::Ge;
using testing::IsTrue;
using testing::Le;
using testing::Lt;

/// How long a case holds its work before letting it complete: long enough that a wait that
/// rechecks on a timer looks many times meanwhile, and well inside the system's own timeout for
/// command buffers that make no progress.
constexpr std::chrono::milliseconds kHeldFor{200};

/// How long after the release a wait may take to return: far longer than waking takes, and far
/// shorter than the five-second budget a wait that missed its wake would sleep to.
constexpr std::chrono::milliseconds kWakeSlack{1000};

/// Most looks a wait for one held command buffer takes: one when it starts, one when the command
/// buffer completes and one when its serial is published, plus one spurious wakeup.
constexpr uint64_t kMaxLooks = 4;

/// Milliseconds from \p from to \p to. @param from Start. @param to End.
int64_t MillisecondsBetween(std::chrono::steady_clock::time_point from,
                            std::chrono::steady_clock::time_point to) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

/// Runs \p release on a thread of its own once \ref kHeldFor has passed, and records when.
class DelayedRelease {
public:
  /// @param release What lets the held work complete.
  template <typename Release>
  explicit DelayedRelease(Release release)
      : thread_([this, release] {
          std::this_thread::sleep_for(kHeldFor);
          releasedAtTicks_.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                 std::memory_order_release);
          release();
        }) {}

  ~DelayedRelease() { join(); }

  /// Waits for the release to have run.
  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// When the release ran. Call after \ref join.
  std::chrono::steady_clock::time_point releasedAt() const {
    return std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(releasedAtTicks_.load(std::memory_order_acquire)));
  }

private:
  std::atomic<std::chrono::steady_clock::rep> releasedAtTicks_{0};
  std::thread thread_;
};

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

/// A wait for held work looks at the completion state only when it changes. A wait that rechecks
/// every millisecond instead looks about once per millisecond the work is held, and on macOS each
/// of those sleeps runs long, which puts a floor of over a millisecond under every wait on the
/// GPU however soon its work completes.
TEST_F(MetalSerialWaitTest, AWaitForHeldWorkLooksOnlyWhenCompletionStateChanges) {
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const uint64_t serial = submitEmptyWork();

  const uint64_t looksBefore = device_->serialWaitLooksForTest();
  DelayedRelease releaser([this] { device_->releasePausedCommandBuffersForTest(1); });
  const bool completed = device_->waitForSerial(serial, /*timeoutSeconds=*/5.0);
  const auto returnedAt = std::chrono::steady_clock::now();
  releaser.join();
  const uint64_t looks = device_->serialWaitLooksForTest() - looksBefore;

  EXPECT_THAT(completed, IsTrue());
  EXPECT_THAT(MillisecondsBetween(releaser.releasedAt(), returnedAt), Ge(0))
      << "the wait returned before its work was released";
  EXPECT_THAT(MillisecondsBetween(releaser.releasedAt(), returnedAt), Lt(kWakeSlack.count()))
      << "the wait slept toward its budget after its work completed";
  EXPECT_THAT(looks, Le(kMaxLooks)) << "the wait looked at the completion state " << looks
                                    << " times while its work was held for " << kHeldFor.count()
                                    << " ms, so it rechecks on a timer instead of sleeping until "
                                       "the work completes";
}

/// Publishing the serial wakes the wait on its own. The command buffer has completed, which also
/// wakes a waiter, but its serial is held back, so publishing it later is the only wake the wait
/// gets: a wait that missed it would sleep until its budget ran out.
TEST_F(MetalSerialWaitTest, PublishingTheSerialWakesTheWait) {
  device_->holdNextCompletionForTest();
  const uint64_t serial = submitEmptyWork();
  ASSERT_THAT(device_->waitForCompletionHandlersForTest(1, /*timeoutSeconds=*/5.0), IsTrue())
      << "the command buffer's completion handler never ran";

  DelayedRelease publisher([this] { device_->releaseHeldCompletionForTest(); });
  const bool completed = device_->waitForSerial(serial, /*timeoutSeconds=*/5.0);
  const auto returnedAt = std::chrono::steady_clock::now();
  publisher.join();

  EXPECT_THAT(completed, IsTrue());
  EXPECT_THAT(MillisecondsBetween(publisher.releasedAt(), returnedAt), Ge(0))
      << "the wait returned before its serial was published";
  EXPECT_THAT(MillisecondsBetween(publisher.releasedAt(), returnedAt), Lt(kWakeSlack.count()))
      << "publishing the serial did not wake the wait, which slept toward its budget";
}

}  // namespace
}  // namespace donner::gpu::metal
