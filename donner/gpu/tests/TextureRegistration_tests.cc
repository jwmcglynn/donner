/// @file
/// Cross-device texture registration: identity, lifetime, ordering and loss, exercised over a
/// test backend whose two devices share a fake native device and whose completion is driven by
/// the test.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::Eq;
using testing::HasSubstr;
using testing::IsFalse;
using testing::IsTrue;
using testing::Lt;

namespace donner::gpu {
namespace {

/// Backend families: only devices of one family may name each other's textures.
constexpr char kSharingFamily = 0;
constexpr char kOtherFamily = 1;

/// A native device two runtime devices can share, counting the native textures still alive.
struct FakeNativeDevice {
  std::atomic<int> liveTextures{0};
};

/// One native texture allocation; alive while any device slot, export or registration holds it.
class FakeNativeTexture {
public:
  explicit FakeNativeTexture(FakeNativeDevice& device) : device_(device) {
    device_.liveTextures.fetch_add(1);
  }
  ~FakeNativeTexture() { device_.liveTextures.fetch_sub(1); }

  FakeNativeTexture(const FakeNativeTexture&) = delete;
  FakeNativeTexture& operator=(const FakeNativeTexture&) = delete;

private:
  FakeNativeDevice& device_;
};

/// The test backend's export: a reference to the native texture.
class FakeExportedTexture final : public ExportedTextureBacking {
public:
  explicit FakeExportedTexture(std::shared_ptr<FakeNativeTexture> native)
      : native(std::move(native)) {}

  std::shared_ptr<FakeNativeTexture> native;
};

/// Completion the test drives, shared with every export of the device that owns it.
class FakeCompletion final : public SubmissionCompletion {
public:
  uint64_t completedSerial() const override { return completed.load(); }
  bool failed() const override { return failedFlag.load(); }

  std::atomic<uint64_t> completed{0};
  std::atomic<bool> failedFlag{false};
};

/// What a test device is built with.
struct SharingOptions {
  const void* family = &kSharingFamily;                     //!< Backend family tag.
  SourceOrdering ordering = SourceOrdering::WaitForSource;  //!< Ordering its exports report.
  std::shared_ptr<DeviceLostState> lostState;               //!< Shared loss condition, or null.
};

/**
 * Test backend whose devices can share textures over one \ref FakeNativeDevice. Submissions
 * complete at once unless the test holds them, and the test decides whether a texture write waits
 * for the next submission.
 */
class SharingDevice final : public Device {
public:
  SharingDevice(FakeNativeDevice& native, SharingOptions options = {})
      : native_(native), options_(std::move(options)) {
    adoptLostState(options_.lostState);
  }

  /// Submissions stop completing until \ref releaseCompletion.
  void holdCompletion() { held_ = true; }
  /// Completes everything submitted so far, and later submissions as they arrive.
  void releaseCompletion() {
    held_ = false;
    completion_->completed.store(lastSubmittedSerial());
  }
  /// Reports a terminal execution failure, as a backend does for a failed command buffer.
  void failExecution() { completion_->failedFlag.store(true); }
  /// Whether a texture write waits for the next submission.
  void setWritesPending(bool pending) { writesPending_ = pending; }
  /// How many explicit backing releases reached this backend.
  int explicitBackingReleases() const { return explicitBackingReleases_; }

  uint64_t completedSerial() const override { return completion_->completed.load(); }

protected:
  BackendDeviceIdentity backendDeviceIdentity() const override {
    return {options_.family, &native_};
  }
  Result<BackendTextureExport> onExportTexture(uint32_t slotIndex) override {
    BackendTextureExport exported;
    exported.backing = std::make_shared<const FakeExportedTexture>(textures_.at(slotIndex));
    exported.ordering = options_.ordering;
    exported.completion = completion_;
    return exported;
  }
  Status onRegisterTexture(uint32_t slotIndex, const ExportedTextureBacking& backing) override {
    slot(slotIndex) = static_cast<const FakeExportedTexture&>(backing).native;
    return OkStatus();
  }
  bool onTextureWritePending(uint32_t) const override { return writesPending_; }
  void onDestroyTextureBacking(uint32_t slotIndex) override {
    ++explicitBackingReleases_;
    slot(slotIndex).reset();
  }

  Status onCreateBuffer(uint32_t, const BufferDescriptor&) override { return OkStatus(); }
  Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor&) override {
    slot(slotIndex) = std::make_shared<FakeNativeTexture>(native_);
    return OkStatus();
  }
  Status onCreateTextureView(uint32_t, uint32_t, const TextureViewDescriptor&) override {
    return OkStatus();
  }
  Status onCreateSampler(uint32_t, const SamplerDescriptor&) override { return OkStatus(); }
  Status onCreateBindGroupLayout(uint32_t, const BindGroupLayoutDescriptor&) override {
    return OkStatus();
  }
  Status onCreateBindGroup(uint32_t, const BindGroupDescriptor&) override { return OkStatus(); }
  Status onCreatePipelineLayout(uint32_t, const PipelineLayoutDescriptor&) override {
    return OkStatus();
  }
  Status onCreateShaderModule(uint32_t, const ShaderModuleDescriptor&) override {
    return OkStatus();
  }
  Status onCreateRenderPipeline(uint32_t, const RenderPipelineDescriptor&) override {
    return OkStatus();
  }
  Status onCreateComputePipeline(uint32_t, const ComputePipelineDescriptor&) override {
    return OkStatus();
  }
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override {
    if (resourceName == TextureTag::kName) {
      slot(slotIndex).reset();
    }
  }
  Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override { return OkStatus(); }
  Status onWriteTexture(uint32_t, std::span<const uint8_t>, const TexelCopyBufferLayout&,
                        const Extent2d&, const Origin2d&) override {
    return OkStatus();
  }
  Status onSubmit(uint64_t submissionSerial, std::span<const SubmittedCommandBuffer>) override {
    if (!held_) {
      completion_->completed.store(submissionSerial);
    }
    return OkStatus();
  }

private:
  std::shared_ptr<FakeNativeTexture>& slot(uint32_t slotIndex) {
    if (slotIndex >= textures_.size()) {
      textures_.resize(slotIndex + 1);
    }
    return textures_[slotIndex];
  }

  FakeNativeDevice& native_;
  SharingOptions options_;
  std::shared_ptr<FakeCompletion> completion_ = std::make_shared<FakeCompletion>();
  std::vector<std::shared_ptr<FakeNativeTexture>> textures_;
  bool held_ = false;
  bool writesPending_ = false;
  int explicitBackingReleases_ = 0;
};

constexpr Extent2d kExtent{4, 4};
constexpr uint64_t kTextureBytes = 4u * 4u * 4u;
constexpr TextureUsage kProducerUsage = TextureUsage::RenderAttachment | TextureUsage::Sampled |
                                        TextureUsage::CopySrc | TextureUsage::CopyDst;

/// Creates a readable, writable test texture on \p device.
/// @param device Device to allocate on. @param usage Usage to create with.
Texture MakeTexture(Device& device, TextureUsage usage = kProducerUsage) {
  return GetResultOrFail(
      device.createTexture(TextureDescriptor{"shared", kExtent, TextureFormat::RGBA8Unorm, usage}));
}

/// Submits one copy of \p texture into a fresh buffer, which is a submission that reads it.
/// @param device Device \p texture belongs to. @param texture Texture to read.
Result<uint64_t> SubmitRead(Device& device, const Texture& texture) {
  Result<Buffer> buffer = device.createBuffer(BufferDescriptor{
      "readback", 256u * kExtent.height, BufferUsage::CopyDst | BufferUsage::MapRead});
  if (buffer.hasError()) {
    return std::move(buffer).error();
  }
  Result<std::unique_ptr<CommandEncoder>> encoder = device.createCommandEncoder();
  if (encoder.hasError()) {
    return std::move(encoder).error();
  }
  if (Status copied = encoder.result()->copyTextureToBuffer(
          TexelCopyTextureInfo{texture}, buffer.result(),
          TexelCopyBufferLayout{0, 256, kExtent.height}, kExtent);
      copied.hasError()) {
    return std::move(copied).error();
  }
  Result<CommandBuffer> commands = encoder.result()->finish();
  if (commands.hasError()) {
    return std::move(commands).error();
  }
  return device.submit(std::move(commands).result());
}

class TextureRegistrationTest : public testing::Test {
protected:
  FakeNativeDevice native_;
  std::unique_ptr<SharingDevice> producer_ = std::make_unique<SharingDevice>(native_);
  std::unique_ptr<SharingDevice> consumer_ = std::make_unique<SharingDevice>(native_);
};

/// The export is made on the producer's thread against its own table, so the handle is checked
/// there: a null handle, one whose texture is gone, and one belonging to another device.
TEST_F(TextureRegistrationTest, ExportResolvesTheHandleAgainstTheProducersTable) {
  EXPECT_THAT(producer_->exportTexture(Texture()), IsGpuError(GpuErrorType::InvalidHandle));

  Texture retired = MakeTexture(*producer_);
  const Texture stale =
      Texture::CreateForBackend(retired.slotIndex(), retired.generation(), retired.deviceId());
  ASSERT_THAT(producer_->destroyTexture(std::move(retired)), IsOk());
  EXPECT_THAT(producer_->exportTexture(stale), IsGpuError(GpuErrorType::InvalidHandle));

  const Texture consumers = MakeTexture(*consumer_);
  EXPECT_THAT(producer_->exportTexture(consumers), IsGpuError(GpuErrorType::DeviceMismatch));
}

/// A registration is exported from the device that allocated it, never passed along, and a lost
/// device has nothing whose contents can be trusted.
TEST_F(TextureRegistrationTest, ExportRefusesARegistrationAndALostProducer) {
  const Texture owned = MakeTexture(*producer_);
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));
  EXPECT_THAT(consumer_->exportTexture(registered), IsGpuError(GpuErrorType::InvalidState));

  producer_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds(1),
                                      "test-injected loss");
  EXPECT_THAT(producer_->exportTexture(owned), IsGpuError(GpuErrorType::DeviceLost));
}

/// A backend whose runtime devices never share a native device refuses both halves by name, which
/// is what the recording backend, like the Vulkan and browser backends, inherits.
TEST_F(TextureRegistrationTest, ABackendThatCannotShareRefusesByName) {
  RecordingDevice recording;
  const Texture texture = MakeTexture(recording);
  EXPECT_THAT(recording.exportTexture(texture),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("one runtime device")));

  const Texture owned = MakeTexture(*producer_);
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));
  EXPECT_THAT(recording.registerTexture(exported), IsGpuError(GpuErrorType::Unsupported));
}

/// Registration reads only the token, and refuses everything it cannot honestly name.
TEST_F(TextureRegistrationTest, RegistrationRefusesWhatItCannotName) {
  EXPECT_THAT(consumer_->registerTexture(TextureExport()), IsGpuError(GpuErrorType::InvalidHandle));

  Texture owned = MakeTexture(*producer_);
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));
  EXPECT_THAT(producer_->registerTexture(exported), IsGpuError(GpuErrorType::InvalidState))
      << "a device's own texture is named by its own handle";

  SharingDevice otherBackend(native_, SharingOptions{.family = &kOtherFamily});
  EXPECT_THAT(otherBackend.registerTexture(exported), IsGpuError(GpuErrorType::DeviceMismatch));

  FakeNativeDevice otherNative;
  SharingDevice otherNativeDevice(otherNative);
  EXPECT_THAT(otherNativeDevice.registerTexture(exported),
              IsGpuError(GpuErrorType::DeviceMismatch));

  ASSERT_THAT(producer_->destroyTexture(std::move(owned)), IsOk());
  EXPECT_THAT(consumer_->registerTexture(exported), IsGpuError(GpuErrorType::InvalidHandle))
      << "a producer that released its handle no longer names the texture";
}

/// Either device's loss ends registration: nothing read through it could be trusted.
TEST_F(TextureRegistrationTest, RegistrationRefusesALostProducerOrConsumer) {
  const Texture owned = MakeTexture(*producer_);
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));

  consumer_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds(1),
                                      "test-injected consumer loss");
  EXPECT_THAT(consumer_->registerTexture(exported), IsGpuError(GpuErrorType::DeviceLost));

  SharingDevice freshConsumer(native_);
  producer_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds(1),
                                      "test-injected producer loss");
  EXPECT_THAT(freshConsumer.registerTexture(exported), IsGpuError(GpuErrorType::DeviceLost));
}

/// A registration describes what the producer describes, limited to reading it: a consumer
/// reads what the producer wrote and never writes it, and it never owns the allocation.
TEST_F(TextureRegistrationTest, ARegistrationIsAReadOnlyDescriptionOfTheProducersTexture) {
  const Texture owned = MakeTexture(*producer_);
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  const TextureDescriptor described = GetResultOrFail(consumer_->textureDescriptor(registered));
  EXPECT_THAT(described.size, Eq(kExtent));
  EXPECT_THAT(described.format, Eq(TextureFormat::RGBA8Unorm));
  EXPECT_THAT(described.label, Eq(RcString("shared")));
  EXPECT_THAT(described.usage, Eq(TextureUsage::Sampled | TextureUsage::CopySrc));
  EXPECT_THAT(consumer_->ownsTextureBacking(registered), IsFalse());

  const std::array<uint8_t, 256 * 4> texels{};
  EXPECT_THAT(consumer_->writeTexture(registered, texels, {0, 256, 4}, kExtent),
              IsGpuError(GpuErrorType::UsageMismatch));

  const Texture writeOnly =
      MakeTexture(*producer_, TextureUsage::RenderAttachment | TextureUsage::CopyDst);
  EXPECT_THAT(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(writeOnly))),
              IsGpuError(GpuErrorType::UsageMismatch));
}

/// The allocation lives exactly as long as its last holder: the producer's explicit release gives
/// up only the producer's hold while a registration still reads it.
TEST_F(TextureRegistrationTest, AnAllocationLivesAsLongAsItsLastHolder) {
  Texture owned = MakeTexture(*producer_);
  std::optional<TextureExport> exported = GetResultOrFail(producer_->exportTexture(owned));
  Texture registered = GetResultOrFail(consumer_->registerTexture(*exported));

  ASSERT_THAT(producer_->destroyTextureBacking(std::move(owned)), IsOk());
  EXPECT_THAT(producer_->explicitBackingReleases(), Eq(0))
      << "an allocation another device reads must not be released under it";
  EXPECT_THAT(native_.liveTextures.load(), Eq(1));

  exported.reset();
  EXPECT_THAT(native_.liveTextures.load(), Eq(1)) << "the registration still holds it";

  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// Sharing changes nothing for a texture nobody else holds: its explicit release is immediate.
TEST_F(TextureRegistrationTest, AnUnsharedTextureStillReleasesItsBackingAtOnce) {
  Texture owned = MakeTexture(*producer_);
  { const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned)); }

  ASSERT_THAT(producer_->destroyTextureBacking(std::move(owned)), IsOk());
  EXPECT_THAT(producer_->explicitBackingReleases(), Eq(1));
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// A registration holds the allocation until the consumer's last submission naming it completes,
/// not merely until its handle is dropped.
TEST_F(TextureRegistrationTest, ARegistrationIsHeldUntilTheConsumersLastUseCompletes) {
  Texture owned = MakeTexture(*producer_);
  Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  consumer_->holdCompletion();
  ASSERT_THAT(SubmitRead(*consumer_, registered), HasResult());
  ASSERT_THAT(producer_->destroyTexture(std::move(owned)), IsOk());
  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  EXPECT_THAT(native_.liveTextures.load(), Eq(1)) << "the consumer's read has not completed";

  consumer_->releaseCompletion();
  consumer_->poll();
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// A registration keeps working after the producer device is gone.
TEST_F(TextureRegistrationTest, ARegistrationOutlivesItsProducerDevice) {
  const Texture owned = MakeTexture(*producer_);
  Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  producer_.reset();
  EXPECT_THAT(native_.liveTextures.load(), Eq(1));
  EXPECT_THAT(SubmitRead(*consumer_, registered), HasResult());

  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// Memory that outlives the producer's handle is still resident, and the producer's gauge says
/// how much, until the last holder lets go.
TEST_F(TextureRegistrationTest, TheTailGaugeCountsWhatOutlivesTheProducersHandle) {
  Texture owned = MakeTexture(*producer_);
  std::optional<TextureExport> exported = GetResultOrFail(producer_->exportTexture(owned));
  Texture registered = GetResultOrFail(consumer_->registerTexture(*exported));
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(0u)) << "the producer still holds it";

  ASSERT_THAT(producer_->destroyTexture(std::move(owned)), IsOk());
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(kTextureBytes));

  exported.reset();
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(kTextureBytes))
      << "the registration still holds it";

  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(0u));
}

/// On separate queues, consumer work naming a registration may not reach its queue before the
/// producer work it follows has completed: submitting early is refused, the bounded wait says
/// so without declaring anything, and once the producer completes both succeed.
TEST_F(TextureRegistrationTest, ConsumerWorkWaitsForTheProducerWorkItFollows) {
  const Texture owned = MakeTexture(*producer_);
  producer_->holdCompletion();
  ASSERT_THAT(SubmitRead(*producer_, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  EXPECT_THAT(SubmitRead(*consumer_, registered),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("waitForTextureSource")));
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsFalse());
  EXPECT_THAT(consumer_->isLost(), IsFalse());
  EXPECT_THAT(producer_->isLost(), IsFalse());

  producer_->releaseCompletion();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 1.0), IsTrue());
  EXPECT_THAT(SubmitRead(*consumer_, registered), HasResult());
}

/// A registration follows the producer's work as of the registration, not as of the export: a
/// texture exported before it was written is still ordered after the write.
TEST_F(TextureRegistrationTest, ARegistrationFollowsProducerWorkAcceptedAfterTheExport) {
  const Texture owned = MakeTexture(*producer_);
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));

  producer_->holdCompletion();
  ASSERT_THAT(SubmitRead(*producer_, owned), HasResult());
  const Texture registered = GetResultOrFail(consumer_->registerTexture(exported));

  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsFalse())
      << "the producer submission after the export is not complete";
  producer_->releaseCompletion();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsTrue());
}

/// A write queued for the producer's next submission has not been ordered anywhere yet, so the
/// texture cannot be registered until a submission carries it; the registration then follows
/// that submission. The runtime never submits on the producer's behalf.
TEST_F(TextureRegistrationTest, AQueuedProducerWriteRefusesRegistrationUntilItIsSubmitted) {
  const Texture owned = MakeTexture(*producer_);
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));
  producer_->setWritesPending(true);
  const std::array<uint8_t, 256 * 4> texels{};
  ASSERT_THAT(producer_->writeTexture(owned, texels, {0, 256, 4}, kExtent), IsOk());

  EXPECT_THAT(consumer_->registerTexture(exported), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_THAT(producer_->lastSubmittedSerial(), Eq(0u));

  producer_->holdCompletion();
  const Texture unrelated = MakeTexture(*producer_);
  ASSERT_THAT(SubmitRead(*producer_, unrelated), HasResult());
  const Texture registered = GetResultOrFail(consumer_->registerTexture(exported));
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsFalse());
  producer_->releaseCompletion();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsTrue());
}

/// Devices that feed one native queue are ordered by submission order alone.
TEST_F(TextureRegistrationTest, ASharedQueueRegistrationNeedsNoWait) {
  SharingDevice producer(native_, SharingOptions{.ordering = SourceOrdering::SharedQueue});
  SharingDevice consumer(native_, SharingOptions{.ordering = SourceOrdering::SharedQueue});
  const Texture owned = MakeTexture(producer);
  producer.holdCompletion();
  ASSERT_THAT(SubmitRead(producer, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer.registerTexture(GetResultOrFail(producer.exportTexture(owned))));

  EXPECT_THAT(consumer.waitForTextureSource(registered, 0.0), IsTrue());
  EXPECT_THAT(SubmitRead(consumer, registered), HasResult());
}

/// Loss ends the wait at once rather than spending its budget, and ends submissions naming the
/// registration.
TEST_F(TextureRegistrationTest, TheSourceWaitEndsAtOnceOnLoss) {
  const Texture owned = MakeTexture(*producer_);
  producer_->holdCompletion();
  ASSERT_THAT(SubmitRead(*producer_, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  producer_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds(1),
                                      "test-injected loss");
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 5.0), IsFalse());
  EXPECT_THAT(std::chrono::steady_clock::now() - start, Lt(std::chrono::seconds(1)));
  EXPECT_THAT(SubmitRead(*consumer_, registered), IsGpuError(GpuErrorType::DeviceLost));
}

/// A producer whose backend reported an execution failure is declared lost by the wait that sees
/// it, with no wait site, because no deadline expired. The consumer's own condition is left to
/// its own waits.
TEST_F(TextureRegistrationTest, AProducerFailureIsDeclaredWithoutAWaitSite) {
  const auto producerLost = std::make_shared<DeviceLostState>();
  SharingDevice producer(native_, SharingOptions{.lostState = producerLost});
  const Texture owned = MakeTexture(producer);
  producer.holdCompletion();
  ASSERT_THAT(SubmitRead(producer, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer.exportTexture(owned))));

  producer.failExecution();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 5.0), IsFalse());
  EXPECT_THAT(std::chrono::steady_clock::now() - start, Lt(std::chrono::seconds(1)));
  EXPECT_THAT(producerLost->lost.load(), IsTrue());
  EXPECT_THAT(producerLost->timedOutSite.load(), Eq(DeviceLostWaitSite::None));
  EXPECT_THAT(consumer_->isLost(), IsFalse());
  EXPECT_THAT(SubmitRead(*consumer_, registered), IsGpuError(GpuErrorType::DeviceLost));
}

/// A wait that spends its budget reports that and nothing more: the budget is the caller's
/// policy, not evidence that the device stopped answering.
TEST_F(TextureRegistrationTest, ATimedOutSourceWaitDeclaresNothing) {
  const Texture owned = MakeTexture(*producer_);
  producer_->holdCompletion();
  ASSERT_THAT(SubmitRead(*producer_, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.02), IsFalse());
  EXPECT_THAT(producer_->isLost(), IsFalse());
  EXPECT_THAT(consumer_->isLost(), IsFalse());
}

/// A texture of the device itself needs no source wait.
TEST_F(TextureRegistrationTest, ADevicesOwnTextureNeedsNoSourceWait) {
  const Texture owned = MakeTexture(*producer_);
  producer_->holdCompletion();
  ASSERT_THAT(SubmitRead(*producer_, owned), HasResult());
  EXPECT_THAT(producer_->waitForTextureSource(owned, 0.0), IsTrue());
}

/// The consumer half never touches the producer device, so each device can stay on its own
/// thread: the producer keeps allocating, submitting and retiring while the consumer registers,
/// reads and drops what it was handed.
TEST_F(TextureRegistrationTest, RegisteringOnAnotherThreadNeverTouchesTheProducer) {
  constexpr int kTextures = 32;
  std::vector<Texture> owned;
  std::vector<TextureExport> exports;
  for (int i = 0; i < kTextures; ++i) {
    owned.push_back(MakeTexture(*producer_));
    exports.push_back(GetResultOrFail(producer_->exportTexture(owned.back())));
  }

  std::atomic<bool> consumerDone{false};
  std::thread producerThread([&] {
    while (!consumerDone.load()) {
      Texture churn = MakeTexture(*producer_);
      EXPECT_THAT(SubmitRead(*producer_, churn), HasResult());
      EXPECT_THAT(producer_->destroyTexture(std::move(churn)), IsOk());
    }
  });
  std::thread consumerThread([&] {
    for (const TextureExport& exported : exports) {
      Texture registered = GetResultOrFail(consumer_->registerTexture(exported));
      EXPECT_THAT(consumer_->waitForTextureSource(registered, 1.0), IsTrue());
      EXPECT_THAT(SubmitRead(*consumer_, registered), HasResult());
      EXPECT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
    }
    consumerDone.store(true);
  });
  consumerThread.join();
  producerThread.join();

  exports.clear();
  owned.clear();
  producer_->poll();
  consumer_->poll();
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

}  // namespace
}  // namespace donner::gpu
