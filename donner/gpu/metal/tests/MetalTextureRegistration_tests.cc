/// @file
/// Cross-device texture registration on Metal: two runtime devices over the system device, each
/// submitting to its own command queue. A consumer reads what the producer rendered, never before
/// the producer's work completes, and keeps reading it after the producer is gone.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal {
namespace {

using testing::Each;
using testing::Eq;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;

/// One row of texels is exactly one copy row pitch, so a readback needs no repacking.
constexpr Extent2d kExtent{kTexelRowPitchAlignment / 4, 4};
constexpr uint64_t kReadbackBytes = uint64_t{kTexelRowPitchAlignment} * kExtent.height;

/// Opaque green, premultiplied RGBA, as a clear color and as the texels it produces.
constexpr std::array<double, 4> kGreenClear = {0.0, 1.0, 0.0, 1.0};
constexpr std::array<uint8_t, 4> kGreen = {0, 255, 0, 255};
/// Opaque blue texels, written from the host.
constexpr std::array<uint8_t, 4> kBlue = {0, 0, 255, 255};

/// Unwraps \p result, failing the test on error. @param result Result to unwrap.
template <typename T>
T Unwrap(Result<T>&& result) {
  return GetResultOrFail(std::move(result));
}

/// Allocates a texture the producer can render into, write and read.
/// @param device Producer device.
Texture MakeProducerTexture(Device& device) {
  return Unwrap(device.createTexture(
      TextureDescriptor{"producerTarget", kExtent, TextureFormat::RGBA8Unorm,
                        TextureUsage::RenderAttachment | TextureUsage::Sampled |
                            TextureUsage::CopySrc | TextureUsage::CopyDst}));
}

/// Records and submits a render pass that clears \p texture to green.
/// @param device Device \p texture belongs to. @param texture Texture to clear.
Result<uint64_t> SubmitGreenClear(Device& device, const Texture& texture) {
  Result<TextureView> view = device.createTextureView(texture, TextureViewDescriptor{"clear"});
  if (view.hasError()) {
    return std::move(view).error();
  }
  std::unique_ptr<CommandEncoder> encoder = Unwrap(device.createCommandEncoder());
  RenderPassDescriptor pass{"greenClear", {}};
  pass.colorAttachments.push_back(
      RenderPassColorAttachment{view.result(), LoadOp::Clear, StoreOp::Store, kGreenClear});
  Result<RenderPassEncoder*> renderPass = encoder->beginRenderPass(pass);
  if (renderPass.hasError()) {
    return std::move(renderPass).error();
  }
  if (Status ended = renderPass.result()->end(); ended.hasError()) {
    return std::move(ended).error();
  }
  return device.submit(Unwrap(encoder->finish()));
}

/// Records and submits a copy of \p texture into \p readback.
/// @param device Device both belong to. @param texture Source. @param readback Destination.
Result<uint64_t> SubmitCopy(Device& device, const Texture& texture, const Buffer& readback) {
  std::unique_ptr<CommandEncoder> encoder = Unwrap(device.createCommandEncoder());
  if (Status copied = encoder->copyTextureToBuffer(
          TexelCopyTextureInfo{texture}, readback,
          TexelCopyBufferLayout{0, kTexelRowPitchAlignment, kExtent.height}, kExtent);
      copied.hasError()) {
    return std::move(copied).error();
  }
  return device.submit(Unwrap(encoder->finish()));
}

/// A host-readable buffer a texture of \ref kExtent is copied into. @param device Owning device.
Buffer MakeReadbackBuffer(Device& device) {
  return Unwrap(device.createBuffer(
      BufferDescriptor{"readback", kReadbackBytes, BufferUsage::CopyDst | BufferUsage::MapRead}));
}

/// Maps \p readback, which a submitted copy fills, and returns its texels, or nothing on failure.
/// @param device Device \p readback belongs to. @param readback Buffer to read.
std::vector<uint8_t> MapTexels(Device& device, const Buffer& readback) {
  BufferMapping mapping = Unwrap(device.mapBufferAsync(readback, MapMode::Read, 0, kReadbackBytes));
  const MapWaitReport report = Unwrap(device.waitForMapping(mapping, MapWaitParams{0.01, 5.0}, {}));
  if (report.outcome != MapWaitOutcome::Ready) {
    ADD_FAILURE() << "readback map did not complete";
    return {};
  }
  const std::span<const uint8_t> bytes = Unwrap(device.mappedBytes(mapping));
  std::vector<uint8_t> texels(bytes.begin(), bytes.end());
  EXPECT_THAT(device.unmapBuffer(std::move(mapping)), IsOk());
  return texels;
}

/// Copies \p texture back to the host on \p device and returns its texels, or nothing on failure.
/// @param device Device \p texture belongs to. @param texture Texture to read.
std::vector<uint8_t> ReadTexels(Device& device, const Texture& texture) {
  const Buffer readback = MakeReadbackBuffer(device);
  if (Result<uint64_t> submitted = SubmitCopy(device, texture, readback); submitted.hasError()) {
    ADD_FAILURE() << "readback copy refused: " << submitted.error();
    return {};
  }
  return MapTexels(device, readback);
}

/// Splits tightly packed RGBA8 bytes into texels, for an all-texels matcher.
/// @param bytes Packed texels.
std::vector<std::array<uint8_t, 4>> Texels(std::span<const uint8_t> bytes) {
  std::vector<std::array<uint8_t, 4>> texels;
  for (size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
    texels.push_back({bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]});
  }
  return texels;
}

class MetalTextureRegistrationTest : public testing::Test {
protected:
  void SetUp() override {
    producer_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(producer_, "Metal cross-device texture registration");
    consumer_ = MetalDevice::Create();
    ASSERT_THAT(consumer_, testing::NotNull());
  }

  void TearDown() override {
    if (producer_) {
      producer_->resumeSubmissionsForTest();
      EXPECT_THAT(producer_->lastErrorForTest(), IsEmpty());
    }
    if (consumer_) {
      EXPECT_THAT(consumer_->lastErrorForTest(), IsEmpty());
    }
  }

  std::unique_ptr<MetalDevice> producer_;
  std::unique_ptr<MetalDevice> consumer_;
};

/// Two devices opened over the system device name one `MTLDevice`, so the consumer can register
/// what the producer rendered and read exactly those texels back through its own queue.
TEST_F(MetalTextureRegistrationTest, ASiblingDeviceReadsWhatTheProducerRendered) {
  const Texture target = MakeProducerTexture(*producer_);
  ASSERT_THAT(SubmitGreenClear(*producer_, target), HasResult());

  const Texture registered =
      Unwrap(consumer_->registerTexture(Unwrap(producer_->exportTexture(target))));
  ASSERT_THAT(registered.isValid(), IsTrue())
      << "a refused registration means the two devices do not share one MTLDevice object";
  ASSERT_THAT(consumer_->waitForTextureSource(registered, 5.0), IsTrue());

  EXPECT_THAT(Texels(ReadTexels(*consumer_, registered)), Each(Eq(kGreen)));
}

/// Each device submits to its own command queue, and submission order on one queue says nothing
/// about another. With the producer's queue held at a gate, the consumer's read of a texture the
/// producer has not rendered yet is accepted without a host wait and held on the device instead:
/// it does not complete while the gate is closed, nothing is declared lost, and once the gate
/// opens the read completes with the producer's pixels.
TEST_F(MetalTextureRegistrationTest, AConsumerReadWaitsOnTheDeviceForAGatedProducer) {
  const Texture target = MakeProducerTexture(*producer_);
  ASSERT_THAT(producer_->pauseSubmissionsForTest(), IsOk());
  ASSERT_THAT(SubmitGreenClear(*producer_, target), HasResult());

  const Texture registered =
      Unwrap(consumer_->registerTexture(Unwrap(producer_->exportTexture(target))));
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.0), IsTrue())
      << "the device orders the read, so the host has nothing to wait for";

  const Buffer readback = MakeReadbackBuffer(*consumer_);
  const Result<uint64_t> copied = SubmitCopy(*consumer_, registered, readback);
  ASSERT_THAT(copied, HasResult()) << "the read was refused instead of ordered on the device";
  EXPECT_THAT(consumer_->waitForSerial(copied.result(), 0.05), IsFalse())
      << "the read ran before the producer's gated work";
  EXPECT_THAT(consumer_->isLost(), IsFalse());
  EXPECT_THAT(producer_->isLost(), IsFalse());

  producer_->resumeSubmissionsForTest();
  ASSERT_THAT(consumer_->waitForSerial(copied.result(), 5.0), IsTrue());
  EXPECT_THAT(Texels(MapTexels(*consumer_, readback)), Each(Eq(kGreen)));
}

/// The registration holds the texture, so it stays readable after the producer device and the
/// export token are gone.
TEST_F(MetalTextureRegistrationTest, ARegistrationOutlivesTheProducerDevice) {
  const Texture registered = [&] {
    const Texture target = MakeProducerTexture(*producer_);
    EXPECT_THAT(SubmitGreenClear(*producer_, target), HasResult());
    Texture result = Unwrap(consumer_->registerTexture(Unwrap(producer_->exportTexture(target))));
    EXPECT_THAT(consumer_->waitForTextureSource(result, 5.0), IsTrue());
    return result;
  }();
  EXPECT_THAT(producer_->lastErrorForTest(), IsEmpty());
  producer_.reset();

  EXPECT_THAT(Texels(ReadTexels(*consumer_, registered)), Each(Eq(kGreen)));
}

/// Devices with separate loss conditions over one `MTLDevice` can still share, and a lost
/// producer ends further registration.
TEST_F(MetalTextureRegistrationTest, SeparateLossConditionsOverOneDeviceStillShare) {
  const auto producerLost = std::make_shared<DeviceLostState>();
  const std::unique_ptr<MetalDevice> producer =
      MetalDevice::Create(MetalDevice::MemoryModel::Detected, kMaxBufferByteSize,
                          std::chrono::seconds(5), producerLost);
  ASSERT_THAT(producer, testing::NotNull());
  const Texture target = MakeProducerTexture(*producer);
  ASSERT_THAT(SubmitGreenClear(*producer, target), HasResult());
  const TextureExport exported = Unwrap(producer->exportTexture(target));

  const Texture registered = Unwrap(consumer_->registerTexture(exported));
  ASSERT_THAT(consumer_->waitForTextureSource(registered, 5.0), IsTrue());
  EXPECT_THAT(Texels(ReadTexels(*consumer_, registered)), Each(Eq(kGreen)));

  ASSERT_THAT(DeclareDeviceLost(*producerLost), IsTrue());
  EXPECT_THAT(consumer_->registerTexture(exported), IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(consumer_->isLost(), IsFalse());
}

/// A host write to a texture earlier work still uses is queued for the producer's next
/// submission. Until a submission carries it, nothing orders it before a consumer, so the texture
/// cannot be registered; afterwards the registration follows that submission and reads the write.
TEST_F(MetalTextureRegistrationTest, AQueuedWriteRefusesRegistrationUntilItIsSubmitted) {
  const Texture target = MakeProducerTexture(*producer_);
  ASSERT_THAT(producer_->pauseSubmissionsForTest(), IsOk());
  ASSERT_THAT(SubmitGreenClear(*producer_, target), HasResult());

  std::vector<uint8_t> blue(kReadbackBytes);
  for (size_t offset = 0; offset < blue.size(); offset += 4) {
    std::copy(kBlue.begin(), kBlue.end(), blue.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  ASSERT_THAT(
      producer_->writeTexture(target, blue, {0, kTexelRowPitchAlignment, kExtent.height}, kExtent),
      IsOk());
  ASSERT_THAT(producer_->writeStatsForTest().pendingWrites, Eq(1u))
      << "the write must be queued for this case to mean anything";

  const TextureExport exported = Unwrap(producer_->exportTexture(target));
  EXPECT_THAT(consumer_->registerTexture(exported), IsGpuError(GpuErrorType::InvalidState));

  ASSERT_THAT(producer_->submit(Unwrap(Unwrap(producer_->createCommandEncoder())->finish())),
              HasResult());
  const Texture registered = Unwrap(consumer_->registerTexture(exported));
  EXPECT_THAT(consumer_->waitForTextureSource(registered, 0.05), IsFalse());

  producer_->resumeSubmissionsForTest();
  ASSERT_THAT(consumer_->waitForTextureSource(registered, 5.0), IsTrue());
  EXPECT_THAT(Texels(ReadTexels(*consumer_, registered)), Each(Eq(kBlue)));
}

}  // namespace
}  // namespace donner::gpu::metal
