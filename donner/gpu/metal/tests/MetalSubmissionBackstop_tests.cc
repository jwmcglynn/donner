/// @file
/// The Metal backend's backstop on command buffers in flight: a submission that would leave more
/// than \ref donner::gpu::metal::MetalDevice::kMaxCommandBuffersInFlight uncompleted waits for
/// room, gives up on a GPU that stops completing work, and never blocks inside the native queue.
/// A serial wait that gives up only on a stall judges it on the same progress clock.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal {
namespace {

using testing::Eq;
using testing::Ge;
using testing::HasSubstr;
using testing::Le;
using testing::Lt;
using testing::Not;

constexpr uint64_t kBackstop = MetalDevice::kMaxCommandBuffersInFlight;
constexpr uint64_t kFullSize = Device::kMaxCommandBuffersPerSubmission;
/// Full-size submissions that fill the backstop exactly.
constexpr uint64_t kFillingSubmissions = kBackstop / kFullSize;
static_assert(kFillingSubmissions * kFullSize == kBackstop);
/// A stall bound short enough to reach well before the system's own timeout ends a command buffer
/// that makes no progress, which would otherwise declare the loss first.
constexpr std::chrono::milliseconds kShortStallBound{200};
/// Longest any case keeps work paused, measured from just before its first paused commit. Well
/// inside the system's own timeout for command buffers that make no progress, about five seconds,
/// so a case whose submission is stuck releases the paused work itself, and fails, before the
/// system ends that work and disturbs other GPU work on the machine.
constexpr std::chrono::milliseconds kReleaseDeadline{3000};
/// Releases every paused command buffer.
constexpr uint64_t kReleaseAll = std::numeric_limits<uint64_t>::max();

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

/// Submits one empty command buffer, doing nothing when the root is lost meanwhile.
/// @param device Device to submit to.
void SubmitEmptyWorkUnlessLost(MetalDevice& device) {
  Result<std::unique_ptr<CommandEncoder>> encoder = device.createCommandEncoder();
  if (!encoder.hasResult()) {
    return;
  }
  Result<CommandBuffer> commandBuffer = encoder.result()->finish();
  if (commandBuffer.hasResult()) {
    (void)device.submit(std::move(commandBuffer).result());
  }
}

/// What a submission run on a worker thread reported.
struct WorkerSubmission {
  bool finishedInWindow = false;           //!< Whether it returned before its work was released.
  std::optional<Result<uint64_t>> result;  //!< What it returned.
  int64_t waitedMs = 0;                    //!< How long it took.
  std::chrono::steady_clock::time_point startedAt;  //!< When it was submitted.
  std::chrono::steady_clock::time_point endedAt;    //!< When it returned.
};

/// Runs \p submit on a worker thread while the calling thread waits for it until \p releaseAt. A
/// submission still running then is let go by \p unblock, which releases the GPU work it is held
/// behind, so a case reports it instead of hanging.
/// @param submit Submission to run. @param unblock Releases every paused command buffer the
///   submission could be held behind. @param releaseAt When \p unblock runs at the latest.
WorkerSubmission SubmitOnWorker(const std::function<Result<uint64_t>()>& submit,
                                const std::function<void()>& unblock,
                                std::chrono::steady_clock::time_point releaseAt) {
  std::mutex mutex;
  std::condition_variable finishedChanged;
  bool finished = false;
  WorkerSubmission outcome;
  std::thread worker([&] {
    const auto start = std::chrono::steady_clock::now();
    Result<uint64_t> result = submit();
    const auto end = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex);
    outcome.result.emplace(std::move(result));
    outcome.waitedMs = MillisecondsBetween(start, end);
    outcome.startedAt = start;
    outcome.endedAt = end;
    finished = true;
    finishedChanged.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    outcome.finishedInWindow =
        finishedChanged.wait_until(lock, releaseAt, [&] { return finished; });
  }
  if (!outcome.finishedInWindow) {
    unblock();
  }
  worker.join();
  return outcome;
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
  }

  void TearDown() override {
    if (device_) {
      ExpectSettledWithoutSystemTimeout(*device_);
    }
  }

  /// Pauses the GPU and commits command buffers up to the backstop, none of which can complete
  /// until they are released. Records in \ref fillStarted_ a time no later than the device's last
  /// progress before the fill.
  void fillBackstopWhilePaused() {
    ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
    fillStarted_ = std::chrono::steady_clock::now();
    for (uint64_t submission = 0; submission < kFillingSubmissions; ++submission) {
      ASSERT_THAT(SubmitEmptyWork(*device_, kFullSize), HasResult());
    }
    EXPECT_THAT(device_->commandBuffersInFlightForTest(), Eq(kBackstop));
  }

  /// Submits \p buffers empty command buffers on a worker thread, releasing all paused work by
  /// \ref kReleaseDeadline after the fill began; see \ref SubmitOnWorker.
  /// @param buffers Command buffers the submission carries.
  WorkerSubmission submitPastTheBackstop(uint64_t buffers = 1) {
    return SubmitOnWorker([&] { return SubmitEmptyWork(*device_, buffers); },
                          [&] { device_->releasePausedCommandBuffersForTest(kReleaseAll); },
                          fillStarted_ + kReleaseDeadline);
  }

  std::shared_ptr<DeviceLostState> rootLoss_ = std::make_shared<DeviceLostState>();
  std::unique_ptr<MetalDevice> device_;
  /// When the fill began. The stall bound runs from the device's last progress, which can be no
  /// earlier, since the device was idle until the fill.
  std::chrono::steady_clock::time_point fillStarted_;
};

/// Work that stays below the backstop never waits for room, so ordinary frames cost nothing extra,
/// and every command buffer it committed is counted back out once it completes.
TEST_F(MetalSubmissionBackstopTest, WorkWithinTheBackstopNeverWaits) {
  for (int frame = 0; frame < 8; ++frame) {
    uint64_t lastSerial = 0;
    for (int submission = 0; submission < 3; ++submission) {
      lastSerial = GetResultOrFail(SubmitEmptyWork(*device_, 2));
    }
    ASSERT_TRUE(device_->waitForSerial(lastSerial, /*timeoutSeconds=*/30.0))
        << device_->lastErrorForTest();
  }
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(0u));
  EXPECT_THAT(device_->commandBuffersInFlightForTest(), Eq(0u));
}

/// A submission that would take the command buffers in flight past the backstop waits until
/// enough of them complete, then goes through.
TEST_F(MetalSubmissionBackstopTest, ASubmissionPastTheBackstopWaitsForRoom) {
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  std::chrono::steady_clock::time_point releasedAt;
  std::thread releaser([&] {
    // Released only once the submission is waiting for room, however late its worker starts, and
    // in any case well before the paused work's release deadline.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (device_->commandBufferRoomWaitsForTest() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    releasedAt = std::chrono::steady_clock::now();
    device_->releasePausedCommandBuffersForTest(1);
  });
  const WorkerSubmission past = submitPastTheBackstop();
  releaser.join();

  EXPECT_TRUE(past.finishedInWindow) << "the submission past the backstop never went through";
  ASSERT_TRUE(past.result.has_value());
  EXPECT_THAT(*past.result, HasResult());
  EXPECT_TRUE(past.endedAt >= releasedAt)
      << "the submission went through before any room was made, so it did not wait for room";
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(1u));
  EXPECT_THAT(device_->commandBuffersInFlightForTest(), Le(kBackstop));
}

/// A GPU that completes nothing while a submission waits for room is treated as hung once the
/// stall bound passes: the root is declared lost, attributed to the queue wait, and the submission
/// is refused without consuming a serial. It never blocks inside the native queue, where no bound
/// applies.
TEST_F(MetalSubmissionBackstopTest, AStalledGpuDeclaresTheRootLostInsteadOfBlocking) {
  device_->setSubmissionStallTimeoutForTest(kShortStallBound);
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  const WorkerSubmission past = submitPastTheBackstop();

  EXPECT_TRUE(past.finishedInWindow)
      << "the submission blocked inside the native queue while the GPU made no progress";
  ASSERT_TRUE(past.result.has_value());
  EXPECT_THAT(*past.result, IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(MillisecondsBetween(fillStarted_, past.endedAt), Ge(kShortStallBound.count()))
      << "the submission gave up before the GPU had made no progress for its stall bound";
  EXPECT_THAT(past.waitedMs, Lt(kShortStallBound.count() + 1000))
      << "the submission waited well past its stall bound";
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

  std::thread declarer([&] {
    // Declared only once the submission is waiting for room, so the declaration has a wait to end,
    // and in any case well before the paused work is released.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (device_->commandBufferRoomWaitsForTest() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)DeclareDeviceLost(*rootLoss_);
  });
  const WorkerSubmission past = submitPastTheBackstop();
  declarer.join();

  EXPECT_TRUE(past.finishedInWindow) << "the wait for room outlived a loss declared during it";
  ASSERT_TRUE(past.result.has_value());
  EXPECT_THAT(*past.result, IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::None))
      << "the wait for room claimed a loss it did not observe";
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(1u))
      << "the submission never waited for room, so no wait was ended";
}

/// The stall bound measures time without progress, not time spent waiting: while command buffers
/// keep completing, a few at a time, a submission that waits longer than the bound in all is still
/// committed and the root is kept.
TEST_F(MetalSubmissionBackstopTest, CommandBuffersCompletingKeepALongWaitAlive) {
  constexpr std::chrono::milliseconds kStallBound{1000};
  constexpr std::chrono::milliseconds kReleaseInterval{150};
  // Released this many per interval, room for a full-size submission takes longer than the stall
  // bound to appear.
  constexpr uint64_t kReleasedPerInterval = kFullSize / 8;
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  std::thread releaser([&] {
    for (uint64_t released = kReleasedPerInterval; released <= kFullSize;
         released += kReleasedPerInterval) {
      std::this_thread::sleep_for(kReleaseInterval);
      device_->releasePausedCommandBuffersForTest(released);
    }
  });
  const WorkerSubmission past = submitPastTheBackstop(kFullSize);
  releaser.join();

  EXPECT_TRUE(past.finishedInWindow);
  ASSERT_TRUE(past.result.has_value());
  EXPECT_THAT(*past.result, HasResult()) << "a wait that saw command buffers complete gave up";
  EXPECT_THAT(past.waitedMs, Ge(kStallBound.count()))
      << "the submission did not wait longer than the stall bound, so this case measured nothing";
  EXPECT_FALSE(device_->isLost()) << "a wait that saw command buffers complete declared a loss";
  EXPECT_THAT(device_->commandBufferRoomWaitsForTest(), Eq(1u));
}

/// One wait for room is capped in all, however much progress it sees: a GPU that completes work
/// too slowly to ever make room ends the wait at the cap with the root declared lost.
TEST_F(MetalSubmissionBackstopTest, TheWaitCapEndsAWaitThatKeepsProgressing) {
  constexpr std::chrono::milliseconds kCap{600};
  constexpr std::chrono::milliseconds kReleaseInterval{100};
  // Room for a full-size submission would take sixteen intervals, well past the cap.
  constexpr uint64_t kReleasedPerInterval = kFullSize / 16;
  device_->setSubmissionStallTimeoutForTest(std::chrono::milliseconds(1000));
  device_->setSubmissionWaitCapForTest(kCap);
  ASSERT_NO_FATAL_FAILURE(fillBackstopWhilePaused());

  std::thread releaser([&] {
    // Stops once the root is lost, so the rest of the paused work is let go at once rather than
    // held toward the system's own timeout for stalled command buffers.
    for (uint64_t released = kReleasedPerInterval;
         released <= kFullSize && !rootLoss_->lost.load(std::memory_order_acquire);
         released += kReleasedPerInterval) {
      std::this_thread::sleep_for(kReleaseInterval);
      device_->releasePausedCommandBuffersForTest(released);
    }
  });
  const WorkerSubmission past = submitPastTheBackstop(kFullSize);
  releaser.join();

  EXPECT_TRUE(past.finishedInWindow);
  ASSERT_TRUE(past.result.has_value());
  EXPECT_THAT(*past.result, IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(past.waitedMs, Ge(kCap.count())) << "the wait ended before its cap";
  EXPECT_THAT(past.waitedMs, Lt(kCap.count() + 1000)) << "the wait ran well past its cap";
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
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
  }

  void TearDown() override {
    if (producer_) {
      ExpectSettledWithoutSystemTimeout(*producer_);
    }
    MetalSubmissionBackstopTest::TearDown();
  }

  /// Pauses the producer and submits \p earlier empty submissions followed by one that renders the
  /// returned texture, so the texture becomes readable only after every one of them has run.
  /// @param earlier Producer submissions ahead of the one that renders the texture.
  Texture pausedProducerTexture(uint64_t earlier) {
    Texture texture = GetResultOrFail(producer_->createTexture(TextureDescriptor{
        "producer target", Extent2d{64, 4}, TextureFormat::RGBA8Unorm,
        TextureUsage::RenderAttachment | TextureUsage::Sampled | TextureUsage::CopySrc}));
    EXPECT_THAT(producer_->pauseSubmissionsForTest(), IsOk());
    producerPausedAt_ = std::chrono::steady_clock::now();
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

  /// Lets the paused producer's command buffers run one at a time, \p interval apart, from a
  /// thread of their own: \p releases of them.
  /// @param releases How many to let run. @param interval Time between releases.
  PacedSteps releaseProducer(int releases, std::chrono::milliseconds interval) {
    return PacedSteps(releases, interval, [this](int released) {
      producer_->releasePausedCommandBuffersForTest(static_cast<uint64_t>(released));
    });
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

  /// Submits one empty consumer command buffer on a worker thread; a submission still held
  /// \ref kReleaseDeadline after the producer paused is let go by releasing the producer.
  WorkerSubmission submitConsumerPastTheBackstop() {
    return SubmitOnWorker([&] { return SubmitEmptyWork(*device_); },
                          [&] { producer_->releasePausedCommandBuffersForTest(kReleaseAll); },
                          producerPausedAt_ + kReleaseDeadline);
  }

  std::unique_ptr<MetalDevice> producer_;
  /// When the producer paused, just before its first paused commit.
  std::chrono::steady_clock::time_point producerPausedAt_;
};

/// A consumer whose waiting work is held behind another device keeps waiting while that device
/// keeps completing work, even when the consumer itself completes nothing for longer than its
/// stall bound: the work it waits on is progressing, so its GPU is not hung.
TEST_F(MetalSubmissionBackstopProducerTest, AProducerThatKeepsCompletingKeepsAConsumerWaitAlive) {
  // Several release intervals fit inside the stall bound, so a slow or shared GPU still shows the
  // producer's progress in time, and ten of them pass before the consumer can complete anything,
  // well before the paused work's release deadline.
  constexpr std::chrono::milliseconds kStallBound{700};
  constexpr std::chrono::milliseconds kReleaseInterval{80};
  constexpr uint64_t kEarlier = 10;
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  const Texture target = pausedProducerTexture(kEarlier);
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  ASSERT_NO_FATAL_FAILURE(fillConsumerBehind(source));

  std::thread releaser([&] {
    // The producer's command buffers run one per interval, the first at once and the texture's
    // last, so the consumer sees no completion of its own for longer than its stall bound.
    for (uint64_t released = 1; released <= kEarlier + 1; ++released) {
      producer_->releasePausedCommandBuffersForTest(released);
      if (released <= kEarlier) {
        std::this_thread::sleep_for(kReleaseInterval);
      }
    }
  });
  const WorkerSubmission past = submitConsumerPastTheBackstop();
  releaser.join();

  EXPECT_TRUE(past.finishedInWindow)
      << "the consumer was still waiting when the paused work was released; it submitted "
      << MillisecondsBetween(producerPausedAt_, past.startedAt) << " ms after the producer paused";
  ASSERT_TRUE(past.result.has_value());
  EXPECT_THAT(*past.result, HasResult()) << "a wait behind a progressing producer gave up";
  EXPECT_THAT(past.waitedMs, Ge(kStallBound.count()))
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
  constexpr std::chrono::milliseconds kReleaseInterval{80};
  constexpr uint64_t kEarlier = 10;
  const Texture target = pausedProducerTexture(kEarlier);
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  const uint64_t serial = SubmitReadOf(*device_, source);

  PacedSteps release = releaseProducer(static_cast<int>(kEarlier + 1), kReleaseInterval);
  const SerialWaitResult waited = device_->waitForSerialUnlessStalled(serial, kStallBound);
  release.join();
  ASSERT_THAT(release, KeptPaceWithin(kStallBound));

  EXPECT_THAT(waited.end, Eq(SerialWaitEnd::Completed))
      << "a wait behind a producer that kept completing gave up: " << waited;
  EXPECT_THAT(waited.waited.count(), Ge(kStallBound.count()))
      << "the consumer's work completed within the stall bound, so this case measured nothing";
  EXPECT_FALSE(device_->isLost());
}

/// A serial wait behind a producer that stops completing gives up within the stall bound of the
/// producer's last progress, and declares nothing: what a stall means is the caller's decision.
TEST_F(MetalSubmissionBackstopProducerTest,
       ASerialWaitBehindAProducerThatStopsEndsWithinItsStallBound) {
  constexpr std::chrono::milliseconds kStallBound{500};
  constexpr std::chrono::milliseconds kReleaseInterval{80};
  constexpr uint64_t kEarlier = 10;
  constexpr uint64_t kReleased = 3;
  const Texture target = pausedProducerTexture(kEarlier);
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  const uint64_t serial = SubmitReadOf(*device_, source);

  PacedSteps release = releaseProducer(static_cast<int>(kReleased), kReleaseInterval);
  const SerialWaitResult waited = device_->waitForSerialUnlessStalled(serial, kStallBound);
  const auto endedAt = std::chrono::steady_clock::now();
  release.join();
  ASSERT_THAT(release, KeptPaceWithin(kStallBound));

  // The wait cannot have seen the last progress before its release began, so the lower bound is
  // measured from there, and the upper bound from when the release returned.
  EXPECT_THAT(waited.end, Eq(SerialWaitEnd::Stalled)) << waited;
  EXPECT_THAT(MillisecondsBetween(release.lastStepStart(), endedAt), Ge(kStallBound.count()))
      << "the wait gave up while the producer was still completing work";
  EXPECT_THAT(MillisecondsBetween(release.lastStepEnd(), endedAt), Lt(kStallBound.count() + 1000))
      << "the wait outlived its stall bound after the producer stopped";
  EXPECT_FALSE(device_->isLost()) << "the serial wait declared a loss itself";
  producer_->releasePausedCommandBuffersForTest(kReleaseAll);
}

/// Once the producer completes the work a consumer's submissions wait for, the producer's later
/// work is unrelated to them: a consumer whose own work does not complete declares the loss once
/// its stall bound passes, however busy the producer stays.
TEST_F(MetalSubmissionBackstopProducerTest,
       AProducerBusyWithUnrelatedWorkDoesNotKeepAConsumerWaitAlive) {
  // Many unrelated submissions fit inside the stall bound, so a slow or shared machine still
  // completes several of them during the consumer's wait.
  constexpr std::chrono::milliseconds kStallBound{500};
  constexpr std::chrono::milliseconds kBusyInterval{25};
  device_->setSubmissionStallTimeoutForTest(kStallBound);
  const Texture target = pausedProducerTexture(0);
  // The serial the consumer's work waits for; anything the producer completes after it is
  // unrelated to the consumer.
  const uint64_t awaited = producer_->lastSubmittedSerial();
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  // The consumer's own work stays paused, so only the producer can show progress.
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  fillStarted_ = std::chrono::steady_clock::now();
  ASSERT_NO_FATAL_FAILURE(fillConsumerBehind(source));

  std::promise<void> unrelatedWorkCompleted;
  std::future<void> unrelatedWorkCompletedFuture = unrelatedWorkCompleted.get_future();
  std::atomic<bool> stopBusyProducer{false};
  uint64_t completedBeforeLoss = 0;
  std::thread busyProducer([&] {
    // The producer renders the texture at once, which meets the consumer's waits, then keeps
    // completing unrelated work. It stops once the root is lost, when the consumer's paused work
    // is released, or when the case gives up on it.
    producer_->resumeSubmissionsForTest();
    const auto until = fillStarted_ + kReleaseDeadline;
    bool announced = false;
    while (!rootLoss_->lost.load(std::memory_order_acquire) &&
           !stopBusyProducer.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < until) {
      completedBeforeLoss = producer_->completedSerial();
      if (!announced && completedBeforeLoss > awaited) {
        unrelatedWorkCompleted.set_value();
        announced = true;
      }
      SubmitEmptyWorkUnlessLost(*producer_);
      std::this_thread::sleep_for(kBusyInterval);
    }
  });
  // The wait starts only once the awaited serial is met and unrelated producer work has completed.
  if (unrelatedWorkCompletedFuture.wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
    stopBusyProducer.store(true, std::memory_order_release);
    busyProducer.join();
    FAIL() << "the producer completed no unrelated work within 1 s";
  }
  const uint64_t completedAtWaitStart = producer_->completedSerial();
  const WorkerSubmission past = submitPastTheBackstop();
  busyProducer.join();
  const uint64_t unrelatedCompletedDuringWait =
      completedBeforeLoss > completedAtWaitStart ? completedBeforeLoss - completedAtWaitStart : 0;

  EXPECT_TRUE(past.finishedInWindow) << "a producer busy with work the consumer no longer waits "
                                        "for kept the consumer's wait alive";
  ASSERT_TRUE(past.result.has_value());
  EXPECT_THAT(*past.result, IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(MillisecondsBetween(fillStarted_, past.endedAt), Ge(kStallBound.count()))
      << "the consumer gave up before its own work had made no progress for its stall bound";
  EXPECT_THAT(past.waitedMs, Lt(kStallBound.count() + 1000))
      << "the consumer waited well past its stall bound";
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
  EXPECT_THAT(unrelatedCompletedDuringWait, Ge(2u))
      << "the producer completed too little unrelated work during the wait for this case to show "
         "that it no longer counts";
}

/// A consumer held behind a producer that completes nothing declares the loss once its stall
/// bound passes: neither it nor the work it waits on is making progress.
TEST_F(MetalSubmissionBackstopProducerTest, AProducerThatStopsLetsTheConsumerWaitDeclareTheLoss) {
  device_->setSubmissionStallTimeoutForTest(kShortStallBound);
  const Texture target = pausedProducerTexture(0);
  const Texture source =
      GetResultOrFail(device_->registerTexture(GetResultOrFail(producer_->exportTexture(target))));
  ASSERT_NO_FATAL_FAILURE(fillConsumerBehind(source));

  const WorkerSubmission past = submitConsumerPastTheBackstop();
  // The consumer's blocked work is released through the producer it waits on.
  producer_->releasePausedCommandBuffersForTest(kReleaseAll);

  EXPECT_TRUE(past.finishedInWindow)
      << "the consumer blocked inside the native queue behind a producer that made no progress";
  ASSERT_TRUE(past.result.has_value());
  EXPECT_THAT(*past.result, IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(past.waitedMs, Lt(kShortStallBound.count() + 1000));
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::QueueIdle));
}

}  // namespace
}  // namespace donner::gpu::metal
