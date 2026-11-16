/// @file
/// The Metal backend's backstop on command buffers in flight: a submission that would leave more
/// than \ref donner::gpu::metal::MetalDevice::kMaxCommandBuffersInFlight uncompleted waits for
/// room, gives up on a GPU that stops completing work, and never blocks inside the native queue.
/// A serial wait that gives up only on a stall judges it on the same progress clock.
///
/// Every case but one runs its devices on a \ref donner::gpu::metal::TestProgressClock and moves
/// it by hand, so each bound is met at the events the case orders: how long a loaded machine takes
/// to run the GPU work or to schedule the case's threads does not change what a wait decides. One
/// case runs the wait for room on the steady clock, as production does, and checks its outcome
/// only.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "donner/base/tests/StallGuard.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/metal/tests/MetalWaitTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal {
namespace {

using testing::Eq;
using testing::Gt;
using testing::HasSubstr;
using testing::IsTrue;
using testing::Le;
using testing::Not;

constexpr uint64_t kBackstop = MetalDevice::kMaxCommandBuffersInFlight;
constexpr uint64_t kFullSize = Device::kMaxCommandBuffersPerSubmission;
/// Full-size submissions that fill the backstop exactly.
constexpr uint64_t kFillingSubmissions = kBackstop / kFullSize;
static_assert(kFillingSubmissions * kFullSize == kBackstop);
/// Releases every paused command buffer.
constexpr uint64_t kReleaseAll = std::numeric_limits<uint64_t>::max();
/// The smallest step a case moves the progress clock by: a case stops this short of a bound to
/// show that a wait is still pending just before it, then moves the clock onto the bound.
constexpr std::chrono::milliseconds kTick{1};
/// How far a case that ends early moves the progress clock to let every wait go: past every bound
/// any case sets.
constexpr std::chrono::hours kPastEveryBound{1};

/// Opens a device sharing \p rootLoss, as a selected backend opens its devices, so a case can read
/// which wait declared a loss. @param rootLoss Loss condition of the root.
std::unique_ptr<MetalDevice> CreateOverRoot(std::shared_ptr<DeviceLostState> rootLoss) {
  return MetalDevice::Create(MetalDevice::MemoryModel::Detected, kMaxBufferByteSize,
                             std::chrono::seconds(5), std::move(rootLoss));
}

/// One finished command buffer with no commands. @param device Device to record it on.
CommandBuffer EmptyCommandBuffer(MetalDevice& device) {
  return GetResultOrFail(GetResultOrFail(device.createCommandEncoder())->finish());
}

/// Submits \p buffers empty command buffers as one submission.
/// @param device Device to submit to. @param buffers Command buffers the submission carries.
Result<uint64_t> SubmitEmptyWork(MetalDevice& device, uint64_t buffers = 1) {
  std::vector<CommandBuffer> commandBuffers;
  commandBuffers.reserve(buffers);
  for (uint64_t index = 0; index < buffers; ++index) {
    commandBuffers.push_back(EmptyCommandBuffer(device));
  }
  return device.submit(std::span<CommandBuffer>(commandBuffers));
}

/// Lets the first \p released command buffers \p device paused run, and waits until every one of
/// them has completed and stamped its progress. Waits on the count of completed command buffers,
/// which only grows: the level in flight can rise again as soon as the completions make room for a
/// waiting submission, which then commits its own command buffers.
/// @param device Device whose paused work to release.
/// @param released Command buffers released in all, counted from the first one paused.
/// @param completedBefore \ref MetalDevice::commandBuffersCompletedForTest before the first one was
///   paused.
void ReleaseAndAwaitCompletion(MetalDevice& device, uint64_t released, uint64_t completedBefore) {
  device.releasePausedCommandBuffersForTest(released);
  ASSERT_THAT(donner::tests::WaitUntil([&] {
                return device.commandBuffersCompletedForTest() >= completedBefore + released;
              }),
              IsTrue())
      << "the released command buffers never completed: "
      << device.commandBuffersCompletedForTest() - completedBefore << " of " << released
      << " completed";
}

/// Waits until \p device has completed \p serial. @param device Device to watch.
/// @param serial Serial to wait for.
void AwaitCompleted(const MetalDevice& device, uint64_t serial) {
  ASSERT_THAT(donner::tests::WaitUntil([&] { return device.completedSerial() >= serial; }),
              IsTrue())
      << "serial " << serial << " never completed; the last completed is "
      << device.completedSerial();
}

/// Lets the paused work of \p device run, waits for every submission it made to finish, and checks
/// that the system's own timeout ended none of it, which would mean the case held paused work too
/// long, and that every command buffer was counted back out. @param device Device to settle.
void ExpectSettledWithoutSystemTimeout(MetalDevice& device) {
  device.resumeSubmissionsForTest();
  EXPECT_TRUE(device.waitForCompletionHandlersForTest(device.lastSubmittedSerial(),
                                                      /*timeoutSeconds=*/10.0))
      << "submitted work did not finish once released";
  EXPECT_THAT(device.lastErrorForTest(), Not(HasSubstr("kIOGPUCommandBufferCallbackErrorTimeout")))
      << "the system ended paused work, so a case held it past the system's own timeout";
  EXPECT_THAT(device.commandBuffersInFlightForTest(), Eq(0u))
      << "a finished command buffer was never counted back out";
}

class MetalSubmissionBackstopTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = CreateOverRoot(rootLoss_);
    DONNER_REQUIRE_METAL_DEVICE(device_, "the Metal submission backstop");
    if (onTestClock_) {
      ASSERT_THAT(device_->setProgressClockForTest(clock_), IsOk());
    }
  }

  void TearDown() override {
    if (device_) {
      ExpectSettledWithoutSystemTimeout(*device_);
    }
  }

  /// Ends every wait a case's worker can be in, for a case that ends before its worker returns:
  /// lets all paused work run, declares the root lost, and moves the clock past every bound.
  virtual void letEveryWaitEnd() {
    device_->releasePausedCommandBuffersForTest(kReleaseAll);
    (void)DeclareDeviceLost(*rootLoss_);
    clock_->advance(kPastEveryBound);
  }

  /// Pauses the GPU and commits command buffers up to the backstop, none of which can complete
  /// until they are released.
  void fillBackstopWhilePaused() {
    completedBeforeFill_ = device_->commandBuffersCompletedForTest();
    ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
    for (uint64_t submission = 0; submission < kFillingSubmissions; ++submission) {
      ASSERT_THAT(SubmitEmptyWork(*device_, kFullSize), HasResult());
    }
    EXPECT_THAT(device_->commandBuffersInFlightForTest(), Eq(kBackstop));
  }

  /// Submits \p buffers empty command buffers to the case's device on a worker thread.
  /// @param buffers Command buffers the submission carries.
  std::unique_ptr<WaitOnWorker<Result<uint64_t>>> submitPastTheBackstop(uint64_t buffers = 1) {
    return std::make_unique<WaitOnWorker<Result<uint64_t>>>(
        [this, buffers] { return SubmitEmptyWork(*device_, buffers); },
        [this] { letEveryWaitEnd(); });
  }

  /// How the wait on the case's device stood once it had judged its bounds at the clock's current
  /// time. @param worker The worker that waits.
  template <typename T>
  WaitStanding judgedAtNow(const WaitOnWorker<T>& worker) {
    return AwaitJudgedAtNow(*device_, *clock_, [&] { return worker.returned(); });
  }

  /// Time on the progress clock since \p since, in milliseconds. @param since Earlier time.
  int64_t millisecondsSince(std::chrono::steady_clock::time_point since) const {
    return MillisecondsBetween(since, clock_->now());
  }

  std::shared_ptr<DeviceLostState> rootLoss_ = std::make_shared<DeviceLostState>();
  std::shared_ptr<TestProgressClock> clock_ = std::make_shared<TestProgressClock>();
  std::unique_ptr<MetalDevice> device_;
  /// Whether \ref SetUp installs \ref clock_ on the case's devices.
  bool onTestClock_ = true;
  /// \ref MetalDevice::commandBuffersCompletedForTest just before \ref fillBackstopWhilePaused.
  uint64_t completedBeforeFill_ = 0;
};

/// Work that stays below the backstop never waits for room, so ordinary frames cost nothing extra,
/// and every command buffer it committed is counted back out once it completes.
TEST_F(MetalSubmissionBackstopTest, WorkWithinTheBackstopNeverWaits) {
  for (int frame = 0; frame < 8; ++frame) {
    uint64_t lastSerial = 0;
    for (int submission = 0; submission < 3; ++submission) {
      lastSerial = GetResultOrFail(SubmitEmptyWork(*device_, 2));
    }
    // On the worker, because its budget never runs out on a clock that does not move: a wait that
    // missed its completion fails at the hang guard rather than blocking the case.
    const WaitOnWorker<bool> waited(
        [this, lastSerial] { return device_->waitForSerial(lastSerial, /*timeoutSeconds=*/30.0); },
        [this] { letEveryWaitEnd(); });
    ASSERT_THAT(waited.awaitReturn(), IsTrue())
        << "the wait for frame " << frame << "'s work never returned";
    ASSERT_THAT(waited.result(), IsTrue()) << device_->lastErrorForTest();
  }
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(0u));
  EXPECT_THAT(device_->commandBuffersInFlightForTest(), Eq(0u));
}

/// A submission that would take the command buffers in flight past the backstop waits until
/// enough of them complete, then goes through.
TEST_F(MetalSubmissionBackstopTest, ASubmissionPastTheBackstopWaitsForRoom) {
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  const auto past = submitPastTheBackstop();
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the submission went through before any room was made, so it did not wait for room";
  device_->releasePausedCommandBuffersForTest(1);
  ASSERT_THAT(past->awaitReturn(), IsTrue())
      << "the submission past the backstop never went through once room was made";

  EXPECT_THAT(past->result(), HasResult());
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(1u));
  EXPECT_THAT(device_->commandBuffersInFlightForTest(), Le(kBackstop));
}

/// A GPU that completes nothing while a submission waits for room is treated as hung once the
/// stall bound passes: the root is declared lost, attributed to the queue wait, and the submission
/// is refused without consuming a serial. It never blocks inside the native queue, where no bound
/// applies.
TEST_F(MetalSubmissionBackstopTest, AStalledGpuDeclaresTheRootLostInsteadOfBlocking) {
  constexpr std::chrono::milliseconds kStallBound{1000};
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  // The fill is the device's last progress, and the clock has not moved since.
  const auto past = submitPastTheBackstop();
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the submission did not wait for room";
  clock_->advance(kStallBound - kTick);
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the submission gave up before the GPU had made no progress for its stall bound";
  clock_->advance(kTick);
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::Returned))
      << "the submission kept waiting once the GPU had made no progress for its stall bound";

  EXPECT_THAT(past->result(), IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_TRUE(device_->isLost())
      << "a submission that gave up on a stalled GPU left the root usable";
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
  EXPECT_THAT(device_->lastSubmittedSerial(), Eq(kFillingSubmissions))
      << "the refused submission consumed a serial";
}

/// A loss declared over the root while a submission waits for room ends the wait at once, and is
/// reported as that loss rather than as the wait's own timeout.
TEST_F(MetalSubmissionBackstopTest, ALossDeclaredWhileWaitingForRoomEndsTheWait) {
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  const auto past = submitPastTheBackstop();
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the submission did not wait for room, so no wait was ended";
  // The clock never moves, so the declaration is the only thing that can end the wait.
  (void)DeclareDeviceLost(*rootLoss_);
  ASSERT_THAT(past->awaitReturn(), IsTrue())
      << "the wait for room outlived a loss declared during it";

  EXPECT_THAT(past->result(), IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::None))
      << "the wait for room claimed a loss it did not observe";
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(1u));
}

/// The stall bound measures time without progress, not time spent waiting: while command buffers
/// keep completing, a few at a time, a submission that waits longer than the bound in all is still
/// committed and the root is kept.
TEST_F(MetalSubmissionBackstopTest, CommandBuffersCompletingKeepALongWaitAlive) {
  constexpr std::chrono::milliseconds kStallBound{1000};
  // Each group completes less than a stall bound after the last, and room for a full-size
  // submission takes all eight groups, several stall bounds in all.
  constexpr std::chrono::milliseconds kBetweenGroups{600};
  constexpr uint64_t kGroup = kFullSize / 8;
  constexpr uint64_t kGroups = kFullSize / kGroup;
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  const auto waitStart = clock_->now();
  const auto past = submitPastTheBackstop(kFullSize);
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the submission did not wait for room";
  for (uint64_t group = 1; group <= kGroups; ++group) {
    clock_->advance(kBetweenGroups);
    ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
        << "the wait ended " << millisecondsSince(waitStart) << " ms in, after " << group - 1
        << " groups of command buffers had completed, the last " << kBetweenGroups.count()
        << " ms before";
    ASSERT_NO_FATAL_FAILURE(
        ReleaseAndAwaitCompletion(*device_, group * kGroup, completedBeforeFill_));
  }
  ASSERT_THAT(past->awaitReturn(), IsTrue())
      << "the submission never went through once room was made";

  EXPECT_THAT(past->result(), HasResult()) << "a wait that saw command buffers complete gave up";
  EXPECT_THAT(millisecondsSince(waitStart), Gt(kStallBound.count()))
      << "the wait did not last longer than the stall bound, so this case measured nothing";
  EXPECT_FALSE(device_->isLost()) << "a wait that saw command buffers complete declared a loss";
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(1u));
}

/// One wait for room is capped in all, however much progress it sees: a GPU that completes work
/// too slowly to ever make room ends the wait at the cap with the root declared lost.
TEST_F(MetalSubmissionBackstopTest, TheWaitCapEndsAWaitThatKeepsProgressing) {
  constexpr std::chrono::milliseconds kStallBound{1000};
  constexpr std::chrono::milliseconds kCap{600};
  constexpr std::chrono::milliseconds kBetweenGroups{100};
  // Room for a full-size submission would take sixteen groups; the cap comes after five.
  constexpr uint64_t kGroup = kFullSize / 16;
  constexpr uint64_t kGroupsBeforeCap = kCap / kBetweenGroups - 1;
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  device_->setSubmissionWaitCapForTest(kCap);
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  const auto waitStart = clock_->now();
  const auto past = submitPastTheBackstop(kFullSize);
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the submission did not wait for room";
  for (uint64_t group = 1; group <= kGroupsBeforeCap; ++group) {
    clock_->advance(kBetweenGroups);
    ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
        << "the wait ended " << millisecondsSince(waitStart) << " ms in, before its cap";
    ASSERT_NO_FATAL_FAILURE(
        ReleaseAndAwaitCompletion(*device_, group * kGroup, completedBeforeFill_));
  }
  clock_->advance(kBetweenGroups - kTick);
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the wait ended " << millisecondsSince(waitStart) << " ms in, before its cap";
  clock_->advance(kTick);
  ASSERT_THAT(millisecondsSince(waitStart), Eq(kCap.count()));
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::Returned)) << "the wait ran past its cap";

  EXPECT_THAT(past->result(),
              IsGpuErrorWithMessage(GpuErrorType::DeviceLost,
                                    HasSubstr("for room among the command buffers")))
      << "the wait did not end at its cap";
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
}

/// The same device on the steady clock, as production runs it.
class MetalSubmissionBackstopSteadyClockTest : public MetalSubmissionBackstopTest {
protected:
  MetalSubmissionBackstopSteadyClockTest() { onTestClock_ = false; }
};

/// On the steady clock, as in production, a GPU that completes nothing while a submission waits
/// for room ends the wait with the root declared lost. Checks the outcome only: the cases on the
/// test clock check when each bound is met.
TEST_F(MetalSubmissionBackstopSteadyClockTest, AStalledGpuEndsTheWaitWithTheRootLost) {
  // Short, so the paused work stays well inside the system's own timeout for command buffers that
  // make no progress.
  device_->setSubmissionStallTimeoutForTest(std::chrono::milliseconds(200));
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  const auto past = submitPastTheBackstop();
  ASSERT_THAT(past->awaitReturn(), IsTrue())
      << "the submission blocked behind a GPU that made no progress";

  EXPECT_THAT(past->result(), IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_TRUE(device_->isLost())
      << "a submission that gave up on a stalled GPU left the root usable";
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(1u));
}

/// Two devices over one root, as a renderer's context and a context that reads what it rendered.
class MetalSubmissionBackstopProducerTest : public MetalSubmissionBackstopTest {
protected:
  void SetUp() override {
    MetalSubmissionBackstopTest::SetUp();
    if (testing::Test::IsSkipped() || testing::Test::HasFatalFailure()) {
      return;
    }
    producer_ = CreateOverRoot(rootLoss_);
    ASSERT_NE(producer_, nullptr);
    if (onTestClock_) {
      ASSERT_THAT(producer_->setProgressClockForTest(clock_), IsOk());
    }
  }

  void TearDown() override {
    if (producer_) {
      ExpectSettledWithoutSystemTimeout(*producer_);
    }
    MetalSubmissionBackstopTest::TearDown();
  }

  void letEveryWaitEnd() override {
    producer_->releasePausedCommandBuffersForTest(kReleaseAll);
    MetalSubmissionBackstopTest::letEveryWaitEnd();
  }

  /// Pauses the producer and submits \p earlier empty submissions followed by one that renders the
  /// returned texture, so the texture becomes readable only after every one of them has run. The
  /// first of them takes the serial after \ref producerBaseSerial_.
  /// @param earlier Producer submissions ahead of the one that renders the texture.
  Texture pausedProducerTexture(uint64_t earlier) {
    Texture texture = GetResultOrFail(producer_->createTexture(TextureDescriptor{
        "producer target", Extent2d{64, 4}, TextureFormat::RGBA8Unorm,
        TextureUsage::RenderAttachment | TextureUsage::Sampled | TextureUsage::CopySrc}));
    EXPECT_THAT(producer_->pauseSubmissionsForTest(), IsOk());
    producerBaseSerial_ = producer_->lastSubmittedSerial();
    for (uint64_t submission = 0; submission < earlier; ++submission) {
      EXPECT_THAT(SubmitEmptyWork(*producer_), HasResult());
    }
    const TextureView view =
        GetResultOrFail(producer_->createTextureView(texture, TextureViewDescriptor{"target"}));
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(producer_->createCommandEncoder());
    RenderPassDescriptor pass;
    pass.label = "producer clear";
    pass.colorAttachments.push_back(
        RenderPassColorAttachment{view, LoadOp::Clear, StoreOp::Store, {0.0, 1.0, 0.0, 1.0}});
    RenderPassEncoder* renderPass = GetResultOrFail(encoder->beginRenderPass(pass));
    EXPECT_NE(renderPass, nullptr);
    if (renderPass != nullptr) {
      EXPECT_THAT(renderPass->end(), IsOk());
    }
    EXPECT_THAT(producer_->submit(GetResultOrFail(encoder->finish())), HasResult());
    return texture;
  }

  /// Lets the producer's first \p released paused submissions run, one command buffer each, and
  /// waits until they have completed. @param released Submissions released in all.
  void releaseProducerAndAwaitCompletion(uint64_t released) {
    producer_->releasePausedCommandBuffersForTest(released);
    ASSERT_NO_FATAL_FAILURE(AwaitCompleted(*producer_, producerBaseSerial_ + released));
  }

  /// Fills the consumer's backstop with full-size submissions whose first command buffer reads
  /// \p source, so each of them waits on the GPU for the producer to render it.
  /// @param source Consumer's registration of the producer's texture.
  void fillConsumerBehind(const Texture& source) {
    const Buffer readback = GetResultOrFail(device_->createBuffer(
        BufferDescriptor{"readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));
    for (uint64_t submission = 0; submission < kFillingSubmissions; ++submission) {
      std::vector<CommandBuffer> commandBuffers;
      std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
      ASSERT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{source}, readback,
                                               TexelCopyBufferLayout{0, 256, 4}, Extent2d{64, 4}),
                  IsOk());
      commandBuffers.push_back(GetResultOrFail(encoder->finish()));
      while (commandBuffers.size() < kFullSize) {
        commandBuffers.push_back(EmptyCommandBuffer(*device_));
      }
      ASSERT_THAT(device_->submit(std::span<CommandBuffer>(commandBuffers)), HasResult());
    }
    EXPECT_THAT(device_->commandBuffersInFlightForTest(), Eq(kBackstop));
  }

  std::unique_ptr<MetalDevice> producer_;
  /// The producer's last serial before \ref pausedProducerTexture paused it.
  uint64_t producerBaseSerial_ = 0;
};

/// A consumer whose waiting work is held behind another device keeps waiting while that device
/// keeps completing work, even when the consumer itself completes nothing for longer than its
/// stall bound: the work it waits on is progressing, so its GPU is not hung.
TEST_F(MetalSubmissionBackstopProducerTest, AProducerThatKeepsCompletingKeepsAConsumerWaitAlive) {
  constexpr std::chrono::milliseconds kStallBound{700};
  // Each producer submission completes less than a stall bound after the last, and the consumer
  // completes nothing until the producer's eleventh, many stall bounds in all.
  constexpr std::chrono::milliseconds kBetweenReleases{400};
  constexpr uint64_t kEarlier = 10;
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  const Texture target = pausedProducerTexture(kEarlier);
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  ASSERT_NO_FATAL_FAILURE(fillConsumerBehind(source));

  const auto waitStart = clock_->now();
  const auto past = submitPastTheBackstop();
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the consumer did not wait for room";
  for (uint64_t released = 1; released <= kEarlier + 1; ++released) {
    clock_->advance(kBetweenReleases);
    ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
        << "the consumer's wait ended " << millisecondsSince(waitStart) << " ms in, after the "
        << "producer had completed " << released - 1 << " submissions, the last "
        << kBetweenReleases.count() << " ms before";
    ASSERT_NO_FATAL_FAILURE(releaseProducerAndAwaitCompletion(released));
  }
  ASSERT_THAT(past->awaitReturn(), IsTrue())
      << "the consumer never went through once the producer rendered the texture";

  EXPECT_THAT(past->result(), HasResult()) << "a wait behind a progressing producer gave up";
  EXPECT_THAT(millisecondsSince(waitStart), Gt(kStallBound.count()))
      << "the consumer did not wait longer than its stall bound, so this case measured nothing";
  EXPECT_FALSE(device_->isLost()) << "a wait behind a progressing producer declared a loss";
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(1u));
}

/// Submits one consumer submission whose command buffer reads \p source, so it waits on the GPU
/// for the producer to render it. @param device Consumer. @param source Its registration.
/// @return The submission's serial.
uint64_t SubmitReadOf(MetalDevice& device, const Texture& source) {
  const Buffer readback = GetResultOrFail(device.createBuffer(
      BufferDescriptor{"readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
  EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{source}, readback,
                                           TexelCopyBufferLayout{0, 256, 4}, Extent2d{64, 4}),
              IsOk());
  return GetResultOrFail(device.submit(GetResultOrFail(encoder->finish())));
}

/// A serial wait judges a stall on the backstop's progress clock: a consumer whose work is held
/// behind a producer that keeps completing command buffers is waited out, even though the
/// consumer itself completes nothing for longer than the stall bound.
TEST_F(MetalSubmissionBackstopProducerTest,
       ASerialWaitBehindAProducerThatKeepsCompletingOutlastsItsStallBound) {
  constexpr std::chrono::milliseconds kStallBound{700};
  constexpr std::chrono::milliseconds kBetweenReleases{400};
  constexpr uint64_t kEarlier = 10;
  const Texture target = pausedProducerTexture(kEarlier);
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  const uint64_t serial = SubmitReadOf(*device_, source);

  const WaitOnWorker<SerialWaitResult> waited(
      [this, serial, kStallBound] {
        return device_->waitForSerialUnlessStalled(serial, kStallBound);
      },
      [this] { letEveryWaitEnd(); });
  ASSERT_THAT(judgedAtNow(waited), Eq(WaitStanding::StillWaiting))
      << "the wait did not wait for the consumer's work";
  for (uint64_t released = 1; released <= kEarlier + 1; ++released) {
    clock_->advance(kBetweenReleases);
    ASSERT_THAT(judgedAtNow(waited), Eq(WaitStanding::StillWaiting))
        << "the wait ended after the producer had completed " << released - 1
        << " submissions, the last " << kBetweenReleases.count() << " ms before";
    ASSERT_NO_FATAL_FAILURE(releaseProducerAndAwaitCompletion(released));
  }
  ASSERT_THAT(waited.awaitReturn(), IsTrue())
      << "the wait never returned once the producer rendered the texture";

  EXPECT_THAT(waited.result().end, Eq(SerialWaitEnd::Completed))
      << "a wait behind a producer that kept completing gave up: " << waited.result();
  EXPECT_THAT(waited.result().waited.count(),
              Eq((static_cast<int64_t>(kEarlier + 1) * kBetweenReleases).count()))
      << "the wait did not report the time that passed on its progress clock: " << waited.result();
  EXPECT_FALSE(device_->isLost());
}

/// A serial wait behind a producer that stops completing gives up a stall bound after the
/// producer's last progress, not after the wait began, and declares nothing: what a stall means is
/// the caller's decision.
TEST_F(MetalSubmissionBackstopProducerTest,
       ASerialWaitBehindAProducerThatStopsEndsWithinItsStallBound) {
  constexpr std::chrono::milliseconds kStallBound{500};
  constexpr std::chrono::milliseconds kBetweenReleases{100};
  constexpr uint64_t kEarlier = 10;
  constexpr uint64_t kReleased = 3;
  const Texture target = pausedProducerTexture(kEarlier);
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  const uint64_t serial = SubmitReadOf(*device_, source);

  const WaitOnWorker<SerialWaitResult> waited(
      [this, serial, kStallBound] {
        return device_->waitForSerialUnlessStalled(serial, kStallBound);
      },
      [this] { letEveryWaitEnd(); });
  ASSERT_THAT(judgedAtNow(waited), Eq(WaitStanding::StillWaiting))
      << "the wait did not wait for the consumer's work";
  for (uint64_t released = 1; released <= kReleased; ++released) {
    clock_->advance(kBetweenReleases);
    ASSERT_THAT(judgedAtNow(waited), Eq(WaitStanding::StillWaiting))
        << "the wait gave up while the producer was still completing work";
    ASSERT_NO_FATAL_FAILURE(releaseProducerAndAwaitCompletion(released));
  }
  // The producer's last progress was its last release. A stall bound after the wait began has
  // passed by the next look; a stall bound after that progress has not.
  clock_->advance(kStallBound - kTick);
  ASSERT_THAT(judgedAtNow(waited), Eq(WaitStanding::StillWaiting))
      << "the wait gave up less than a stall bound after the producer's last progress";
  clock_->advance(kTick);
  ASSERT_THAT(judgedAtNow(waited), Eq(WaitStanding::Returned))
      << "the wait outlived its stall bound after the producer stopped";

  EXPECT_THAT(waited.result().end, Eq(SerialWaitEnd::Stalled)) << waited.result();
  EXPECT_THAT(waited.result().waited.count(),
              Eq((static_cast<int64_t>(kReleased) * kBetweenReleases + kStallBound).count()))
      << waited.result();
  EXPECT_FALSE(device_->isLost()) << "the serial wait declared a loss itself";
  producer_->releasePausedCommandBuffersForTest(kReleaseAll);
}

/// Once the producer completes the work a consumer's submissions wait for, the producer's later
/// work is unrelated to them: a consumer whose own work does not complete declares the loss once
/// its stall bound passes, however busy the producer stays.
TEST_F(MetalSubmissionBackstopProducerTest,
       AProducerBusyWithUnrelatedWorkDoesNotKeepAConsumerWaitAlive) {
  constexpr std::chrono::milliseconds kStallBound{500};
  // Unrelated producer work completes this often during the consumer's wait, each time less than a
  // stall bound after the meet, so each completion would keep the wait alive if it still counted.
  constexpr std::chrono::milliseconds kBetweenUnrelated{100};
  constexpr int64_t kUnrelated = 3;
  static_assert(kUnrelated * kBetweenUnrelated < kStallBound);
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  const Texture target = pausedProducerTexture(0);
  // The serial the consumer's work waits for; anything the producer completes after it is
  // unrelated to the consumer.
  const uint64_t awaited = producer_->lastSubmittedSerial();
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  // The consumer's own work stays paused, so only the producer can show progress.
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  ASSERT_NO_FATAL_FAILURE(fillConsumerBehind(source));
  // The producer renders the texture, which meets every wait of the consumer's work.
  producer_->resumeSubmissionsForTest();
  ASSERT_NO_FATAL_FAILURE(AwaitCompleted(*producer_, awaited));

  const auto metAt = clock_->now();
  const auto past = submitPastTheBackstop();
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the consumer did not wait for room";
  for (int64_t unrelated = 1; unrelated <= kUnrelated; ++unrelated) {
    clock_->advance(kBetweenUnrelated);
    const uint64_t serial = GetResultOrFail(SubmitEmptyWork(*producer_));
    ASSERT_NO_FATAL_FAILURE(AwaitCompleted(*producer_, serial));
    ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
        << "the consumer gave up " << millisecondsSince(metAt)
        << " ms after the producer met its wait, less than its stall bound";
  }
  ASSERT_THAT(producer_->completedSerial() - awaited, Eq(static_cast<uint64_t>(kUnrelated)))
      << "the producer did not complete the unrelated work this case counts on";

  // Had the unrelated completions counted, the wait would last a stall bound past the last of
  // them; it ends a stall bound past the meet.
  clock_->advance(kStallBound - kUnrelated * kBetweenUnrelated - kTick);
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the consumer gave up " << millisecondsSince(metAt)
      << " ms after the producer met its wait, less than its stall bound";
  clock_->advance(kTick);
  ASSERT_THAT(millisecondsSince(metAt), Eq(kStallBound.count()));
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::Returned))
      << "a producer busy with work the consumer no longer waits for kept the consumer's wait "
         "alive";

  EXPECT_THAT(past->result(), IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
  EXPECT_TRUE(device_->isLost());
}

/// A consumer held behind a producer that completes nothing declares the loss once its stall
/// bound passes: neither it nor the work it waits on is making progress.
TEST_F(MetalSubmissionBackstopProducerTest, AProducerThatStopsLetsTheConsumerWaitDeclareTheLoss) {
  constexpr std::chrono::milliseconds kStallBound{200};
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  const Texture target = pausedProducerTexture(0);
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  ASSERT_NO_FATAL_FAILURE(fillConsumerBehind(source));

  const auto past = submitPastTheBackstop();
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the consumer did not wait for room";
  clock_->advance(kStallBound - kTick);
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::StillWaiting))
      << "the consumer gave up before its stall bound";
  clock_->advance(kTick);
  ASSERT_THAT(judgedAtNow(*past), Eq(WaitStanding::Returned))
      << "the consumer blocked behind a producer that made no progress";

  EXPECT_THAT(past->result(), IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
  // The consumer's blocked work is released through the producer it waits on.
  producer_->releasePausedCommandBuffersForTest(kReleaseAll);
}

}  // namespace
}  // namespace donner::gpu::metal
