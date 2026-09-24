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
#include <initializer_list>
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
#include "donner/gpu/tests/RecordingDeviceObserver.h"
#include "donner/gpu/tests/SharingTestDevice.h"

using testing::ElementsAre;
using testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;
using testing::Lt;

namespace donner::gpu {
namespace {

/// Bytes of one test texture.
constexpr uint64_t kTextureBytes = 4u * 4u * 4u;

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

  Texture retired = MakeSharedTexture(*producer_);
  const Texture stale =
      Texture::CreateForBackend(retired.slotIndex(), retired.generation(), retired.deviceId());
  ASSERT_THAT(producer_->destroyTexture(std::move(retired)), IsOk());
  EXPECT_THAT(producer_->exportTexture(stale), IsGpuError(GpuErrorType::InvalidHandle));

  const Texture consumers = MakeSharedTexture(*consumer_);
  EXPECT_THAT(producer_->exportTexture(consumers), IsGpuError(GpuErrorType::DeviceMismatch));
}

/// A registration is exported from the device that allocated it, never passed along, and a lost
/// device has nothing whose contents can be trusted.
TEST_F(TextureRegistrationTest, ExportRefusesARegistrationAndALostProducer) {
  const Texture owned = MakeSharedTexture(*producer_);
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
  const Texture texture = MakeSharedTexture(recording);
  EXPECT_THAT(recording.exportTexture(texture),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("one runtime device")));

  const Texture owned = MakeSharedTexture(*producer_);
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));
  EXPECT_THAT(recording.registerTexture(exported), IsGpuError(GpuErrorType::Unsupported));
}

/// Registration reads only the token, and refuses everything it cannot honestly name.
TEST_F(TextureRegistrationTest, RegistrationRefusesWhatItCannotName) {
  EXPECT_THAT(consumer_->registerTexture(TextureExport()), IsGpuError(GpuErrorType::InvalidHandle));

  Texture owned = MakeSharedTexture(*producer_);
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
  const Texture owned = MakeSharedTexture(*producer_);
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
  const Texture owned = MakeSharedTexture(*producer_);
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  const TextureDescriptor described = GetResultOrFail(consumer_->textureDescriptor(registered));
  EXPECT_THAT(described.size, Eq(kSharedTextureExtent));
  EXPECT_THAT(described.format, Eq(TextureFormat::RGBA8Unorm));
  EXPECT_THAT(described.label, Eq(RcString("shared")));
  EXPECT_THAT(described.usage, Eq(TextureUsage::Sampled | TextureUsage::CopySrc));
  EXPECT_THAT(consumer_->ownsTextureBacking(registered), IsFalse());

  const std::array<uint8_t, 256 * 4> texels{};
  EXPECT_THAT(consumer_->writeTexture(registered, texels, {0, 256, 4}, kSharedTextureExtent),
              IsGpuError(GpuErrorType::UsageMismatch));

  const Texture writeOnly =
      MakeSharedTexture(*producer_, TextureUsage::RenderAttachment | TextureUsage::CopyDst);
  EXPECT_THAT(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(writeOnly))),
              IsGpuError(GpuErrorType::UsageMismatch));
}

/// The allocation lives exactly as long as its last holder: the producer's explicit release gives
/// up only the producer's hold while a registration still reads it.
TEST_F(TextureRegistrationTest, AnAllocationLivesAsLongAsItsLastHolder) {
  Texture owned = MakeSharedTexture(*producer_);
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
  Texture owned = MakeSharedTexture(*producer_);
  { const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned)); }

  ASSERT_THAT(producer_->destroyTextureBacking(std::move(owned)), IsOk());
  EXPECT_THAT(producer_->explicitBackingReleases(), Eq(1));
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// An export token alone keeps the allocation alive after the producer released its handle, and
/// lets it go with the token.
TEST_F(TextureRegistrationTest, AnExportTokenAloneKeepsTheAllocationAlive) {
  Texture owned = MakeSharedTexture(*producer_);
  std::optional<TextureExport> exported = GetResultOrFail(producer_->exportTexture(owned));

  ASSERT_THAT(producer_->destroyTexture(std::move(owned)), IsOk());
  EXPECT_THAT(native_.liveTextures.load(), Eq(1)) << "the token still holds the allocation";

  exported.reset();
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// A producer that asks for its backing back while another device reads it gets the release
/// when that reader lets go: deferred, never dropped.
TEST_F(TextureRegistrationTest, ADeferredBackingReleaseHappensWhenTheLastHolderLetsGo) {
  Texture owned = MakeSharedTexture(*producer_);
  Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));
  consumer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*consumer_, registered), HasResult());

  ASSERT_THAT(producer_->destroyTextureBacking(std::move(owned)), IsOk());
  EXPECT_THAT(producer_->explicitBackingReleases(), Eq(0));
  EXPECT_THAT(native_.deferredBackingReleases.load(), Eq(0));

  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  EXPECT_THAT(native_.deferredBackingReleases.load(), Eq(0))
      << "the consumer's read has not completed, so its registration still holds the texture";

  consumer_->releaseCompletion();
  consumer_->poll();
  EXPECT_THAT(native_.deferredBackingReleases.load(), Eq(1));
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(0u));
}

/// A producer's ownership of an exported texture ends when it releases its backing, though its own
/// last use of the texture is still running and a registration keeps the allocation alive past
/// both. Its observer hears that once, on the producer's thread, at the release; the tail gauge
/// counts the bytes the registration still holds; and neither the producer's later recycle of the
/// slot nor the registration's final release reports it again. The consumer's registration is not
/// an allocation of the consumer, so its observer hears nothing.
TEST_F(TextureRegistrationTest, AProducersBackingReleaseEndsItsOwnershipOnceWhileItIsStillHeld) {
  tests::RecordingDeviceObserver producerObserver;
  tests::RecordingDeviceObserver consumerObserver;
  const tests::ScopedObserverInstallation producerObserving(*producer_, producerObserver);
  const tests::ScopedObserverInstallation consumerObserving(*consumer_, consumerObserver);
  ASSERT_THAT(producerObserving.status(), IsOk());
  ASSERT_THAT(consumerObserving.status(), IsOk());
  Texture owned = MakeSharedTexture(*producer_);
  Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));
  producer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, owned), HasResult());

  ASSERT_THAT(producer_->destroyTextureBacking(std::move(owned)), IsOk());
  EXPECT_THAT(producerObserver.events.textureReleases, Eq(1u))
      << "the producer's ownership ended at its release, although its own read is still running "
         "and the registration keeps the allocation alive";
  EXPECT_THAT(native_.liveTextures.load(), Eq(1));
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(kTextureBytes));

  producer_->releaseCompletion();
  producer_->poll();
  EXPECT_THAT(producerObserver.events.textureReleases, Eq(1u))
      << "recycling the slot ends no ownership";

  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  consumer_->poll();
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(0u));
  EXPECT_THAT(producerObserver.events.textureReleases, Eq(1u))
      << "the registration's final release frees an allocation the producer no longer owns";
  EXPECT_THAT(producerObserver.events.textureCreates, Eq(1u));
  EXPECT_THAT(consumerObserver.events, Eq(tests::ObservedEvents{}))
      << "registering another device's texture and releasing it allocates and releases nothing";
}

/// When the producer drops its handle instead, its ownership ends when the slot is recycled after
/// its last use completes, reported once there, while the registration keeps the allocation alive.
/// Until that recycle, the tail gauge already counts the bytes the registration holds while the
/// producer's ownership has not been reported ended, so the two overlap.
TEST_F(TextureRegistrationTest, AProducersDroppedHandleEndsItsOwnershipOnceWhileItIsStillHeld) {
  tests::RecordingDeviceObserver producerObserver;
  tests::RecordingDeviceObserver consumerObserver;
  const tests::ScopedObserverInstallation producerObserving(*producer_, producerObserver);
  const tests::ScopedObserverInstallation consumerObserving(*consumer_, consumerObserver);
  ASSERT_THAT(producerObserving.status(), IsOk());
  ASSERT_THAT(consumerObserving.status(), IsOk());
  Texture owned = MakeSharedTexture(*producer_);
  Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));
  producer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, owned), HasResult());

  ASSERT_THAT(producer_->destroyTexture(std::move(owned)), IsOk());
  EXPECT_THAT(producerObserver.events.textureReleases, Eq(0u))
      << "the producer's own read is still running, so it still owns the allocation";
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(kTextureBytes))
      << "the tail gauge counts the held bytes from the producer's retirement, before its "
         "ownership is reported ended at the recycle";

  producer_->releaseCompletion();
  producer_->poll();
  EXPECT_THAT(producerObserver.events.textureReleases, Eq(1u));
  EXPECT_THAT(native_.liveTextures.load(), Eq(1));
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(kTextureBytes));

  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  consumer_->poll();
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
  EXPECT_THAT(producer_->sharedTextureTailBytes(), Eq(0u));
  EXPECT_THAT(producerObserver.events.textureReleases, Eq(1u));
  EXPECT_THAT(producerObserver.events.textureCreates, Eq(1u));
  EXPECT_THAT(consumerObserver.events, Eq(tests::ObservedEvents{}))
      << "registering another device's texture and releasing it allocates and releases nothing";
}

/// A plain destroy of a shared texture is not a request to release its backing early.
TEST_F(TextureRegistrationTest, DestroyingASharedTextureRequestsNoEarlyRelease) {
  Texture owned = MakeSharedTexture(*producer_);
  Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));
  ASSERT_THAT(producer_->destroyTexture(std::move(owned)), IsOk());
  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  EXPECT_THAT(native_.deferredBackingReleases.load(), Eq(0));
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// A registration holds the allocation until the consumer's last submission naming it completes,
/// not merely until its handle is dropped.
TEST_F(TextureRegistrationTest, ARegistrationIsHeldUntilTheConsumersLastUseCompletes) {
  Texture owned = MakeSharedTexture(*producer_);
  Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  consumer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*consumer_, registered), HasResult());
  ASSERT_THAT(producer_->destroyTexture(std::move(owned)), IsOk());
  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  EXPECT_THAT(native_.liveTextures.load(), Eq(1)) << "the consumer's read has not completed";

  consumer_->releaseCompletion();
  consumer_->poll();
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// A registration keeps working after the producer device is gone.
TEST_F(TextureRegistrationTest, ARegistrationOutlivesItsProducerDevice) {
  const Texture owned = MakeSharedTexture(*producer_);
  Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  producer_.reset();
  EXPECT_THAT(native_.liveTextures.load(), Eq(1));
  EXPECT_THAT(SubmitSharedTextureRead(*consumer_, registered), HasResult());

  ASSERT_THAT(consumer_->destroyTexture(std::move(registered)), IsOk());
  EXPECT_THAT(native_.liveTextures.load(), Eq(0));
}

/// Memory that outlives the producer's handle is still resident, and the producer's gauge says
/// how much, until the last holder lets go.
TEST_F(TextureRegistrationTest, TheTailGaugeCountsWhatOutlivesTheProducersHandle) {
  Texture owned = MakeSharedTexture(*producer_);
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
  const Texture owned = MakeSharedTexture(*producer_);
  producer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  EXPECT_THAT(SubmitSharedTextureRead(*consumer_, registered),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("waitForTextureSource")));
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsFalse());
  EXPECT_THAT(consumer_->isLost(), IsFalse());
  EXPECT_THAT(producer_->isLost(), IsFalse());

  producer_->releaseCompletion();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 1.0), IsTrue());
  EXPECT_THAT(SubmitSharedTextureRead(*consumer_, registered), HasResult());
}

/// A registration follows the producer's work as of the registration, not as of the export: a
/// texture exported before it was written is still ordered after the write.
TEST_F(TextureRegistrationTest, ARegistrationFollowsProducerWorkAcceptedAfterTheExport) {
  const Texture owned = MakeSharedTexture(*producer_);
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));

  producer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, owned), HasResult());
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
  const Texture owned = MakeSharedTexture(*producer_);
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));
  producer_->setWritesPending(true);
  const std::array<uint8_t, 256 * 4> texels{};
  ASSERT_THAT(producer_->writeTexture(owned, texels, {0, 256, 4}, kSharedTextureExtent), IsOk());

  EXPECT_THAT(consumer_->registerTexture(exported), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_THAT(producer_->lastSubmittedSerial(), Eq(0u));

  producer_->holdCompletion();
  const Texture unrelated = MakeSharedTexture(*producer_);
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, unrelated), HasResult());
  const Texture registered = GetResultOrFail(consumer_->registerTexture(exported));
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsFalse());
  producer_->releaseCompletion();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsTrue());
}

/// A write the backend queued before the texture was exported is just as unordered: the export
/// reports it, and the next producer submission carries it.
TEST_F(TextureRegistrationTest, AWriteQueuedBeforeTheExportIsCarriedByTheNextSubmission) {
  const Texture owned = MakeSharedTexture(*producer_);
  producer_->setWritesPending(true);
  const std::array<uint8_t, 256 * 4> texels{};
  ASSERT_THAT(producer_->writeTexture(owned, texels, {0, 256, 4}, kSharedTextureExtent), IsOk());
  const TextureExport exported = GetResultOrFail(producer_->exportTexture(owned));
  EXPECT_THAT(consumer_->registerTexture(exported), IsGpuError(GpuErrorType::InvalidState));

  const Texture unrelated = MakeSharedTexture(*producer_);
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, unrelated), HasResult());
  EXPECT_THAT(consumer_->registerTexture(exported), HasResult());
}

/// Devices that feed one native queue are ordered by submission order alone.
TEST_F(TextureRegistrationTest, ASharedQueueRegistrationNeedsNoWait) {
  SharingDevice producer(native_, SharingOptions{.ordering = SourceOrdering::SharedQueue});
  SharingDevice consumer(native_, SharingOptions{.ordering = SourceOrdering::SharedQueue});
  const Texture owned = MakeSharedTexture(producer);
  producer.holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(producer, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer.registerTexture(GetResultOrFail(producer.exportTexture(owned))));

  EXPECT_THAT(consumer.waitForTextureSource(registered, 0.0), IsTrue());
  EXPECT_THAT(SubmitSharedTextureRead(consumer, registered), HasResult());
}

/// Records and submits one command buffer that reads each of \p textures, in order.
/// @param device Device the textures belong to. @param textures Textures to read.
Result<uint64_t> SubmitReads(Device& device, std::initializer_list<const Texture*> textures) {
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
  std::vector<Buffer> buffers;
  for (const Texture* texture : textures) {
    buffers.push_back(GetResultOrFail(
        device.createBuffer(BufferDescriptor{"readback", 256u * kSharedTextureExtent.height,
                                             BufferUsage::CopyDst | BufferUsage::MapRead})));
    if (Status copied = encoder->copyTextureToBuffer(
            TexelCopyTextureInfo{*texture}, buffers.back(),
            TexelCopyBufferLayout{0, 256, kSharedTextureExtent.height}, kSharedTextureExtent);
        copied.hasError()) {
      return std::move(copied).error();
    }
  }
  return device.submit(GetResultOrFail(encoder->finish()));
}

/// Matches a device-side wait for \p serial on some export's backing.
MATCHER_P(WaitsFor, serial,
          "waits on the device for producer serial " + testing::PrintToString(serial)) {
  *result_listener << "waits for serial " << arg.serial
                   << (arg.backing == nullptr ? " on no backing" : "");
  return arg.backing != nullptr && arg.serial == serial;
}

/// Two devices over one native device whose backend orders consumers on the device: work naming a
/// registration is accepted while the producer's work still runs, and the consumer's backend is
/// told which producer work its submission waits for. The host never waits.
class DeviceOrderedRegistrationTest : public testing::Test {
protected:
  FakeNativeDevice native_;
  SharingDevice producer_{native_, SharingOptions{.ordering = SourceOrdering::WaitOnDevice}};
  SharingDevice consumer_{native_, SharingOptions{.ordering = SourceOrdering::WaitOnDevice}};
};

TEST_F(DeviceOrderedRegistrationTest, WorkIsAcceptedAtOnceAndWaitsOnTheDeviceForTheProducer) {
  const Texture owned = MakeSharedTexture(producer_);
  producer_.holdCompletion();
  const uint64_t producerSerial = GetResultOrFail(SubmitSharedTextureRead(producer_, owned));
  const Texture registered =
      GetResultOrFail(consumer_.registerTexture(GetResultOrFail(producer_.exportTexture(owned))));

  EXPECT_THAT(consumer_.waitForTextureSource(registered, 0.0), IsTrue())
      << "the device orders the work, so the host has nothing to wait for";
  ASSERT_THAT(SubmitSharedTextureRead(consumer_, registered), HasResult());
  EXPECT_THAT(consumer_.lastSourceWaits(), ElementsAre(WaitsFor(producerSerial)));
  EXPECT_THAT(consumer_.isLost(), IsFalse());
  EXPECT_THAT(producer_.isLost(), IsFalse());

  producer_.releaseCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(consumer_, registered), HasResult());
  EXPECT_THAT(consumer_.lastSourceWaits(), IsEmpty())
      << "producer work that has completed needs no wait";
}

TEST_F(DeviceOrderedRegistrationTest, ASubmissionWaitsOnceForATextureAtItsLatestSerial) {
  const Texture owned = MakeSharedTexture(producer_);
  const TextureExport exported = GetResultOrFail(producer_.exportTexture(owned));
  producer_.holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(producer_, owned), HasResult());
  const Texture early = GetResultOrFail(consumer_.registerTexture(exported));
  const uint64_t latest = GetResultOrFail(SubmitSharedTextureRead(producer_, owned));
  const Texture late = GetResultOrFail(consumer_.registerTexture(exported));

  ASSERT_THAT(SubmitReads(consumer_, {&early, &late, &early}), HasResult());

  EXPECT_THAT(consumer_.lastSourceWaits(), ElementsAre(WaitsFor(latest)))
      << "one texture is waited for once, at the latest producer work any of its registrations "
         "follows";
}

TEST_F(DeviceOrderedRegistrationTest, ProducerWorkNotYetHandedToItsQueueIsRefusedNotWaitedOn) {
  const Texture owned = MakeSharedTexture(producer_);
  producer_.deferCommit();
  const uint64_t producerSerial = GetResultOrFail(SubmitSharedTextureRead(producer_, owned));
  const Texture registered =
      GetResultOrFail(consumer_.registerTexture(GetResultOrFail(producer_.exportTexture(owned))));

  EXPECT_THAT(SubmitSharedTextureRead(consumer_, registered),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("not submitted")))
      << "a device-side wait on work its producer has not handed over could wait for a host "
         "thread that is waiting for this one";
  EXPECT_THAT(consumer_.waitForTextureSource(registered, 0.0), IsFalse());

  producer_.commitDeferred();
  EXPECT_THAT(consumer_.waitForTextureSource(registered, 0.0), IsTrue());
  ASSERT_THAT(SubmitSharedTextureRead(consumer_, registered), HasResult());
  EXPECT_THAT(consumer_.lastSourceWaits(), ElementsAre(WaitsFor(producerSerial)));
}

TEST_F(DeviceOrderedRegistrationTest, ProducerWorkThatWasOnlyRecordedIsNotWaitedFor) {
  const Texture owned = MakeSharedTexture(producer_);
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(producer_.createCommandEncoder());
  const Buffer target = GetResultOrFail(
      producer_.createBuffer(BufferDescriptor{"recorded", 256u * kSharedTextureExtent.height,
                                              BufferUsage::CopyDst | BufferUsage::MapRead}));
  ASSERT_THAT(encoder->copyTextureToBuffer(
                  TexelCopyTextureInfo{owned}, target,
                  TexelCopyBufferLayout{0, 256, kSharedTextureExtent.height}, kSharedTextureExtent),
              IsOk());
  const CommandBuffer recorded = GetResultOrFail(encoder->finish());
  const Texture registered =
      GetResultOrFail(consumer_.registerTexture(GetResultOrFail(producer_.exportTexture(owned))));

  ASSERT_THAT(SubmitSharedTextureRead(consumer_, registered), HasResult());
  EXPECT_THAT(consumer_.lastSourceWaits(), IsEmpty())
      << "work the producer only recorded follows the registration, so nothing waits for it";
  EXPECT_THAT(recorded.isValid(), IsTrue());
}

TEST_F(DeviceOrderedRegistrationTest, LossAndProducerFailureAreStillRefused) {
  const Texture owned = MakeSharedTexture(producer_);
  producer_.holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(producer_, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_.registerTexture(GetResultOrFail(producer_.exportTexture(owned))));

  producer_.failExecution();
  EXPECT_THAT(consumer_.waitForTextureSource(registered, 0.0), IsFalse());
  EXPECT_THAT(SubmitSharedTextureRead(consumer_, registered), IsGpuError(GpuErrorType::DeviceLost))
      << "a producer whose work failed has nothing a consumer can trust";
  EXPECT_THAT(producer_.isLost(), IsTrue());
}

/// Loss ends the wait at once rather than spending its budget, and ends submissions naming the
/// registration.
TEST_F(TextureRegistrationTest, TheSourceWaitEndsAtOnceOnLoss) {
  const Texture owned = MakeSharedTexture(*producer_);
  producer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  producer_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds(1),
                                      "test-injected loss");
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 5.0), IsFalse());
  EXPECT_THAT(std::chrono::steady_clock::now() - start, Lt(std::chrono::seconds(1)));
  EXPECT_THAT(SubmitSharedTextureRead(*consumer_, registered),
              IsGpuError(GpuErrorType::DeviceLost));
}

/// A producer whose backend reported an execution failure is declared lost by the wait that sees
/// it, with no wait site, because no deadline expired. The consumer's own condition is left to
/// its own waits.
TEST_F(TextureRegistrationTest, AProducerFailureIsDeclaredWithoutAWaitSite) {
  const auto producerLost = std::make_shared<DeviceLostState>();
  SharingDevice producer(native_, SharingOptions{.lostState = producerLost});
  const Texture owned = MakeSharedTexture(producer);
  producer.holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(producer, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer.exportTexture(owned))));

  producer.failExecution();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 5.0), IsFalse());
  EXPECT_THAT(std::chrono::steady_clock::now() - start, Lt(std::chrono::seconds(1)));
  EXPECT_THAT(producerLost->lost.load(), IsTrue());
  EXPECT_THAT(producerLost->timedOutSite.load(), Eq(DeviceLostWaitSite::None));
  EXPECT_THAT(consumer_->isLost(), IsFalse());
  EXPECT_THAT(SubmitSharedTextureRead(*consumer_, registered),
              IsGpuError(GpuErrorType::DeviceLost));
}

/// A wait that spends its budget reports that and nothing more: the budget is the caller's
/// policy, not evidence that the device stopped answering.
TEST_F(TextureRegistrationTest, ATimedOutSourceWaitDeclaresNothing) {
  const Texture owned = MakeSharedTexture(*producer_);
  producer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, owned), HasResult());
  const Texture registered =
      GetResultOrFail(consumer_->registerTexture(GetResultOrFail(producer_->exportTexture(owned))));

  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.02), IsFalse());
  EXPECT_THAT(producer_->isLost(), IsFalse());
  EXPECT_THAT(consumer_->isLost(), IsFalse());
}

/// A surface of \p device whose frames can be sampled and copied from.
/// @param device Device to create it on. @param layer Stand-in for the platform layer.
Surface ConfiguredSurface(Device& device, int& layer) {
  SurfaceDescriptor descriptor;
  descriptor.label = "window";
  descriptor.native.kind = NativeSurfaceKind::MetalLayer;
  descriptor.native.display = &layer;
  Surface surface = GetResultOrFail(device.createSurface(descriptor));
  EXPECT_THAT(device.configureSurface(
                  surface, SurfaceConfiguration{TextureFormat::RGBA8Unorm,
                                                TextureUsage::RenderAttachment |
                                                    TextureUsage::Sampled | TextureUsage::CopySrc,
                                                kSharedTextureExtent, PresentMode::Fifo,
                                                SurfaceAlphaMode::Opaque}),
              IsOk());
  return surface;
}

/// A frame a surface has out goes back to the surface when it is presented. Where readers submit
/// to the producer's own queue, what they record before the present runs before it, so the frame
/// can be exported, as a capture of a surface target needs. Where a reader has its own queue, its
/// read could land after the surface took the frame back, so the frame is refused by name.
TEST_F(TextureRegistrationTest, ASurfaceFrameIsExportedOnlyWhereItsReadersShareItsQueue) {
  int layer = 0;
  SharingDevice sharedProducer(native_, SharingOptions{.ordering = SourceOrdering::SharedQueue});
  SharingDevice sharedConsumer(native_, SharingOptions{.ordering = SourceOrdering::SharedQueue});
  const Surface shared = ConfiguredSurface(sharedProducer, layer);
  const SurfaceTexture frame = GetResultOrFail(sharedProducer.acquireCurrentTexture(shared));
  const TextureExport exported = GetResultOrFail(sharedProducer.exportTexture(frame.texture));
  EXPECT_THAT(sharedConsumer.registerTexture(exported), HasResult());
  EXPECT_THAT(sharedProducer.presentSurface(shared), HasResult());

  const Surface separate = ConfiguredSurface(*producer_, layer);
  const SurfaceTexture held = GetResultOrFail(producer_->acquireCurrentTexture(separate));
  EXPECT_THAT(
      producer_->exportTexture(held.texture),
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("the frame a surface has out")));
  EXPECT_THAT(producer_->abandonCurrentTexture(separate), IsOk());
}

/// The surface takes a presented frame back without the retirement other textures go through,
/// so its export has to be released there too. An export that outlives the present holds the
/// frame's allocation and counts it as released by the producer, and the slot's next frame
/// exports as itself rather than as the frame before it.
TEST_F(TextureRegistrationTest, APresentedFramesExportNeverStandsInForTheNextFrame) {
  int layer = 0;
  SharingDevice producer(native_, SharingOptions{.ordering = SourceOrdering::SharedQueue});
  const Surface surface = ConfiguredSurface(producer, layer);
  const SurfaceTexture first = GetResultOrFail(producer.acquireCurrentTexture(surface));
  std::optional<TextureExport> firstExport = GetResultOrFail(producer.exportTexture(first.texture));
  ASSERT_THAT(producer.presentSurface(surface), HasResult());
  EXPECT_THAT(producer.sharedTextureTailBytes(), Eq(kTextureBytes))
      << "the presented frame is held only by its export";

  const SurfaceTexture second = GetResultOrFail(producer.acquireCurrentTexture(surface));
  const TextureExport secondExport = GetResultOrFail(producer.exportTexture(second.texture));
  firstExport.reset();
  EXPECT_THAT(native_.liveTextures.load(), Eq(1))
      << "dropping the first frame's export frees it, so the second frame's export holds its own "
         "allocation";
  EXPECT_THAT(producer.sharedTextureTailBytes(), Eq(0u));
  EXPECT_THAT(producer.abandonCurrentTexture(surface), IsOk());
}

/// A texture of the device itself needs no source wait.
TEST_F(TextureRegistrationTest, ADevicesOwnTextureNeedsNoSourceWait) {
  const Texture owned = MakeSharedTexture(*producer_);
  producer_->holdCompletion();
  ASSERT_THAT(SubmitSharedTextureRead(*producer_, owned), HasResult());
  EXPECT_THAT(producer_->waitForTextureSource(owned, 0.0), IsTrue());
}

/// The consumer half never touches the producer device, so each device can stay on its own
/// thread. While the consumer registers, orders, reads and drops what it was handed, the producer
/// keeps doing everything a producer does to those same shares: it submits work reading them,
/// queues writes to them that its next submission carries, and releases some of them. Under the
/// thread sanitizer this is the regression for a consumer that reads producer state it does not
/// share through the export.
TEST_F(TextureRegistrationTest, RegisteringOnAnotherThreadNeverTouchesTheProducer) {
  constexpr int kTextures = 32;
  std::vector<Texture> owned;
  std::vector<TextureExport> exports;
  for (int i = 0; i < kTextures; ++i) {
    owned.push_back(MakeSharedTexture(*producer_));
    exports.push_back(GetResultOrFail(producer_->exportTexture(owned.back())));
  }

  std::atomic<bool> consumerDone{false};
  std::thread producerThread([&] {
    producer_->setWritesPending(true);
    const std::array<uint8_t, 256 * 4> texels{};
    for (int round = 0; !consumerDone.load(); ++round) {
      const size_t index = static_cast<size_t>(round) % owned.size();
      if (owned[index].isValid()) {
        EXPECT_THAT(SubmitSharedTextureRead(*producer_, owned[index]), HasResult());
        EXPECT_THAT(
            producer_->writeTexture(owned[index], texels, {0, 256, 4}, kSharedTextureExtent),
            IsOk());
        if (round % 7 == 3) {
          EXPECT_THAT(producer_->destroyTexture(std::move(owned[index])), IsOk());
        }
      }
      Texture churn = MakeSharedTexture(*producer_);
      EXPECT_THAT(SubmitSharedTextureRead(*producer_, churn), HasResult());
      EXPECT_THAT(producer_->destroyTexture(std::move(churn)), IsOk());
    }
  });
  std::thread consumerThread([&] {
    for (int pass = 0; pass < 8; ++pass) {
      for (const TextureExport& exported : exports) {
        Result<Texture> registered = consumer_->registerTexture(exported);
        if (registered.hasError()) {
          // A queued write not yet carried, or a texture the producer has since released.
          EXPECT_THAT(registered.error().type, testing::AnyOf(Eq(GpuErrorType::InvalidState),
                                                              Eq(GpuErrorType::InvalidHandle)));
          continue;
        }
        EXPECT_THAT(consumer_->waitForTextureSource(registered.result(), 1.0), IsTrue());
        EXPECT_THAT(SubmitSharedTextureRead(*consumer_, registered.result()), HasResult());
        EXPECT_THAT(consumer_->destroyTexture(std::move(registered).result()), IsOk());
      }
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
