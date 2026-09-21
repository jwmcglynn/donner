/// @file
/// Multi-command-buffer submission tests: one backend submission and one serial for a span of
/// command buffers, the identity and state refusals that guard it, and the retention every
/// buffer of an accepted span gets until that one serial completes.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::ElementsAre;
using testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;
using testing::SizeIs;

namespace donner::gpu {
namespace {

/// One submission as the backend saw it: its serial and, per command buffer, the slot it
/// occupied and how many commands it carried.
struct ObservedSubmission {
  uint64_t serial = 0;                       //!< Serial the runtime assigned.
  std::vector<uint32_t> commandBufferSlots;  //!< Slots of the buffers, in execution order.
  std::vector<size_t> commandCounts;         //!< Commands per buffer, in execution order.

  /// Equality operator. @param other Submission to compare against.
  bool operator==(const ObservedSubmission& other) const = default;
};

/// Prints an \ref ObservedSubmission so a mismatched submission names its serial and its buffers
/// rather than printing as bytes.
/// @param value Submission to print. @param os Output stream.
void PrintTo(const ObservedSubmission& value, std::ostream* os) {
  *os << "{serial=" << value.serial << " commandBufferSlots=[";
  for (size_t i = 0; i < value.commandBufferSlots.size(); ++i) {
    *os << (i == 0 ? "" : ", ") << value.commandBufferSlots[i] << "(" << value.commandCounts[i]
        << " commands)";
  }
  *os << "]}";
}

/**
 * Test backend that records the submissions it is handed, completes them only when told to, and
 * can refuse one submission the way a device that has just been lost would.
 */
class SubmissionRecordingDevice : public Device {
public:
  /// Submissions the backend received, in the order it received them.
  const std::vector<ObservedSubmission>& submissions() const { return submissions_; }

  /// Backend releases observed, as `<name>#<slot>` strings in release order.
  const std::vector<std::string>& backendReleases() const { return backendReleases_; }

  /// Makes the next submission fail the way a lost device does, without completing anything.
  void failNextSubmission() { failNextSubmission_ = true; }

  /// Marks every submission up to \p serial as executed. @param serial Serial to complete
  /// through.
  void completeUpTo(uint64_t serial) { completedSerial_ = serial; }

  /// Serial of the most recent completed submission.
  uint64_t completedSerial() const override { return completedSerial_; }

protected:
  Status onCreateBuffer(uint32_t, const BufferDescriptor&) override { return OkStatus(); }
  Status onCreateTexture(uint32_t, const TextureDescriptor&) override { return OkStatus(); }
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
    backendReleases_.push_back(std::string(resourceName) + "#" + std::to_string(slotIndex));
  }
  Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override { return OkStatus(); }
  Status onMapBufferAsync(uint32_t, uint32_t, MapMode, uint64_t, uint64_t) override {
    // Enough for the runtime to hold an open mapping; these tests never read one.
    return OkStatus();
  }
  Status onWriteTexture(uint32_t, std::span<const uint8_t>, const TexelCopyBufferLayout&,
                        const Extent2d&, const Origin2d&) override {
    return OkStatus();
  }

  Status onSubmit(uint64_t submissionSerial,
                  std::span<const SubmittedCommandBuffer> commandBuffers) override {
    if (failNextSubmission_) {
      failNextSubmission_ = false;
      return GpuError{GpuErrorType::DeviceLost, "the test backend lost the device"};
    }
    ObservedSubmission observed{submissionSerial, {}, {}};
    for (const SubmittedCommandBuffer& commandBuffer : commandBuffers) {
      observed.commandBufferSlots.push_back(commandBuffer.slotIndex);
      observed.commandCounts.push_back(commandBuffer.commands.size());
    }
    submissions_.push_back(std::move(observed));
    return OkStatus();
  }

private:
  std::vector<ObservedSubmission> submissions_;
  std::vector<std::string> backendReleases_;
  bool failNextSubmission_ = false;
  uint64_t completedSerial_ = 0;
};

/// Records one texture-to-buffer copy per command buffer, so each buffer of a span carries its
/// own resources and a distinguishable command count.
class SubmissionSpanTests : public testing::Test {
protected:
  Texture createTexture() {
    return GetResultOrFail(device_.createTexture(TextureDescriptor{
        "source", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc}));
  }

  Buffer createReadbackBuffer() {
    return GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));
  }

  /// Records \p copyCount copies from \p texture into \p buffer and finishes the encoder.
  CommandBuffer recordCopies(const Texture& texture, const Buffer& buffer, size_t copyCount) {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    for (size_t i = 0; i < copyCount; ++i) {
      EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{texture}, buffer,
                                               TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
                  IsOk());
    }
    return GetResultOrFail(encoder->finish());
  }

  SubmissionRecordingDevice device_;
};

TEST_F(SubmissionSpanTests, SubmitsEveryBufferOfTheSpanAsOneBackendSubmission) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();
  CommandBuffer first = recordCopies(texture, buffer, 1);
  CommandBuffer second = recordCopies(texture, buffer, 2);
  const uint32_t firstSlot = first.slotIndex();
  const uint32_t secondSlot = second.slotIndex();

  std::array<CommandBuffer, 2> span{std::move(first), std::move(second)};
  const Result<uint64_t> serial = device_.submit(span);

  ASSERT_THAT(serial, HasResult());
  EXPECT_THAT(serial.result(), Eq(1u));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(1u));
  EXPECT_THAT(device_.submissions(),
              ElementsAre(ObservedSubmission{1, {firstSlot, secondSlot}, {1, 2}}));
}

TEST_F(SubmissionSpanTests, ConsumesEveryCommandBufferOfAnAcceptedSpan) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();
  std::array<CommandBuffer, 2> span{recordCopies(texture, buffer, 1),
                                    recordCopies(texture, buffer, 1)};

  ASSERT_THAT(device_.submit(span), HasResult());

  EXPECT_THAT(span[0].isValid(), IsFalse());
  EXPECT_THAT(span[1].isValid(), IsFalse());
}

TEST_F(SubmissionSpanTests, RefusesASubmissionThatNamesNoCommandBuffer) {
  const Result<uint64_t> serial = device_.submit(std::span<CommandBuffer>{});

  EXPECT_THAT(serial, IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                            HasSubstr("at least one command buffer")));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(0u));
  EXPECT_THAT(device_.submissions(), IsEmpty());
}

TEST_F(SubmissionSpanTests, RefusesACommandBufferFromAnotherDevice) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();

  SubmissionRecordingDevice other;
  const Texture otherTexture = GetResultOrFail(other.createTexture(TextureDescriptor{
      "source", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc}));
  const Buffer otherBuffer = GetResultOrFail(other.createBuffer(
      BufferDescriptor{"readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));
  std::unique_ptr<CommandEncoder> otherEncoder = GetResultOrFail(other.createCommandEncoder());
  ASSERT_THAT(otherEncoder->copyTextureToBuffer(TexelCopyTextureInfo{otherTexture}, otherBuffer,
                                                TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsOk());

  std::array<CommandBuffer, 2> span{recordCopies(texture, buffer, 1),
                                    GetResultOrFail(otherEncoder->finish())};
  const Result<uint64_t> serial = device_.submit(span);

  EXPECT_THAT(serial, IsGpuError(GpuErrorType::DeviceMismatch));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(0u));
  EXPECT_THAT(device_.submissions(), IsEmpty());
  // The foreign buffer is consumed too, so neither half of the refused span can be submitted
  // again on either device.
  EXPECT_THAT(span[0].isValid(), IsFalse());
  EXPECT_THAT(span[1].isValid(), IsFalse());
  EXPECT_THAT(other.submissions(), IsEmpty());
}

TEST_F(SubmissionSpanTests, RefusesACommandBufferThatWasAlreadySubmitted) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();
  CommandBuffer alreadySubmitted = recordCopies(texture, buffer, 1);
  const uint32_t slotIndex = alreadySubmitted.slotIndex();
  const uint32_t generation = alreadySubmitted.generation();
  ASSERT_THAT(device_.submit(std::move(alreadySubmitted)), HasResult());

  // Submitting consumes the caller's handle, so naming the slot again takes a handle minted with
  // the generation the submitted buffer had. The slot is released and its generation moved on,
  // which is what makes this stale rather than merely null: a null handle would be refused by a
  // check the stale path never reaches.
  // Minted without a device-alive token, so the handle is inert: it names the slot for the
  // refusal below without its destructor releasing whatever occupies that slot by then.
  std::array<CommandBuffer, 1> span{
      CommandBuffer::CreateForBackend(slotIndex, generation, device_.deviceId())};
  ASSERT_THAT(span[0].isValid(), IsTrue());

  EXPECT_THAT(device_.submit(span), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(1u));
  EXPECT_THAT(device_.submissions(), SizeIs(1));
}

TEST_F(SubmissionSpanTests, RefusesANullCommandBuffer) {
  std::array<CommandBuffer, 1> span{CommandBuffer{}};

  EXPECT_THAT(device_.submit(span), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(0u));
  EXPECT_THAT(device_.submissions(), IsEmpty());
}

TEST_F(SubmissionSpanTests, RefusesASpanCarryingMoreBuffersThanOneSubmissionHolds) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();
  std::vector<CommandBuffer> span;
  span.reserve(Device::kMaxCommandBuffersPerSubmission + 1);
  for (size_t i = 0; i <= Device::kMaxCommandBuffersPerSubmission; ++i) {
    span.push_back(recordCopies(texture, buffer, 1));
  }

  EXPECT_THAT(
      device_.submit(span),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                            HasSubstr(std::format("at most {} command buffers",
                                                  Device::kMaxCommandBuffersPerSubmission))));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(0u));
  EXPECT_THAT(device_.submissions(), IsEmpty());

  // Refused on its size alone, before anything was consumed, so the caller still owns every
  // buffer and submits them as spans that fit.
  EXPECT_THAT(span.front().isValid(), IsTrue());
  EXPECT_THAT(span.back().isValid(), IsTrue());
  EXPECT_THAT(
      device_.submit(std::span<CommandBuffer>(span).first(Device::kMaxCommandBuffersPerSubmission)),
      HasResult());
  EXPECT_THAT(device_.submit(std::span<CommandBuffer>(span).last(1)), HasResult());
}

TEST_F(SubmissionSpanTests, ConsumesEveryCommandBufferOfARefusedSpan) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();
  // The refusal lands on the last buffer, after the first two were already taken out of their
  // slots; they stay consumed, or the span could be submitted again with its refused part
  // removed and the accepted part run twice.
  std::array<CommandBuffer, 3> span{recordCopies(texture, buffer, 1),
                                    recordCopies(texture, buffer, 1), CommandBuffer{}};

  EXPECT_THAT(device_.submit(span), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(span[0].isValid(), IsFalse());
  EXPECT_THAT(span[1].isValid(), IsFalse());
  EXPECT_THAT(device_.submit(span), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(0u));
  EXPECT_THAT(device_.submissions(), IsEmpty());
}

TEST_F(SubmissionSpanTests, RefusesASpanWhoseLaterBufferUsesAnOpenMapping) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();
  const Buffer mapped = createReadbackBuffer();
  const BufferMapping mapping =
      GetResultOrFail(device_.mapBufferAsync(mapped, MapMode::Read, 0, 1024));

  // Only the second buffer of the span names the mapped buffer, so the whole span is refused on
  // a use the first buffer never mentions.
  std::array<CommandBuffer, 2> span{recordCopies(texture, buffer, 1),
                                    recordCopies(texture, mapped, 1)};
  EXPECT_THAT(device_.submit(span),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("has an open mapping")));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(0u));
  EXPECT_THAT(device_.submissions(), IsEmpty());
}

TEST_F(SubmissionSpanTests, ASubmissionTheBackendRefusesBurnsNoSerial) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();
  std::array<CommandBuffer, 2> refused{recordCopies(texture, buffer, 1),
                                       recordCopies(texture, buffer, 1)};

  device_.failNextSubmission();
  EXPECT_THAT(device_.submit(refused), IsGpuError(GpuErrorType::DeviceLost));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(0u));

  // The refused span consumed no serial, so the next accepted one is still the first.
  std::array<CommandBuffer, 1> accepted{recordCopies(texture, buffer, 1)};
  const Result<uint64_t> serial = device_.submit(accepted);
  ASSERT_THAT(serial, HasResult());
  EXPECT_THAT(serial.result(), Eq(1u));
}

TEST_F(SubmissionSpanTests, OneSerialCompletesTheWholeSpan) {
  const Texture texture = createTexture();
  const Buffer buffer = createReadbackBuffer();
  std::array<CommandBuffer, 3> span{recordCopies(texture, buffer, 1),
                                    recordCopies(texture, buffer, 1),
                                    recordCopies(texture, buffer, 1)};
  const uint64_t serial = GetResultOrFail(device_.submit(span));

  // Three buffers, one serial: nothing is complete until that serial is, and then all of it is.
  EXPECT_THAT(device_.waitForSerial(serial, 0.0), IsFalse());
  device_.completeUpTo(serial);
  EXPECT_THAT(device_.waitForSerial(serial, 0.0), IsTrue());
  EXPECT_THAT(device_.completedSerial(), Eq(serial));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(serial));
}

TEST_F(SubmissionSpanTests, EveryBufferOfASpanKeepsItsBackingUntilTheSpanCompletes) {
  Texture firstTexture = createTexture();
  Texture secondTexture = createTexture();
  Buffer firstBuffer = createReadbackBuffer();
  Buffer secondBuffer = createReadbackBuffer();
  const std::string firstTextureRelease = "texture#" + std::to_string(firstTexture.slotIndex());
  const std::string secondTextureRelease = "texture#" + std::to_string(secondTexture.slotIndex());
  const std::string firstBufferRelease = "buffer#" + std::to_string(firstBuffer.slotIndex());
  const std::string secondBufferRelease = "buffer#" + std::to_string(secondBuffer.slotIndex());

  std::array<CommandBuffer, 2> span{recordCopies(firstTexture, firstBuffer, 1),
                                    recordCopies(secondTexture, secondBuffer, 1)};
  const uint64_t serial = GetResultOrFail(device_.submit(span));

  // Destroying every resource of both buffers retires the handles, but the one serial pins all
  // of the backing: a device that never completes retires none of it, whichever buffer named it.
  EXPECT_THAT(device_.destroyTexture(std::move(firstTexture)), IsOk());
  EXPECT_THAT(device_.destroyTexture(std::move(secondTexture)), IsOk());
  EXPECT_THAT(device_.destroyBuffer(std::move(firstBuffer)), IsOk());
  EXPECT_THAT(device_.destroyBuffer(std::move(secondBuffer)), IsOk());
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), IsEmpty());

  device_.completeUpTo(serial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(),
              testing::UnorderedElementsAre(firstTextureRelease, secondTextureRelease,
                                            firstBufferRelease, secondBufferRelease));
}

TEST(SubmissionSpanSerialization, RecordsEveryBufferOfTheSpanUnderOneSerial) {
  RecordingDevice device;
  const Texture texture = GetResultOrFail(device.createTexture(TextureDescriptor{
      "source", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc}));
  const Buffer readback = GetResultOrFail(
      device.createBuffer(BufferDescriptor{"readback", 1024, BufferUsage::CopyDst}));

  const auto record = [&](size_t copyCount) {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
    for (size_t i = 0; i < copyCount; ++i) {
      EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{texture}, readback,
                                               TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
                  IsOk());
    }
    return GetResultOrFail(encoder->finish());
  };

  std::array<CommandBuffer, 2> span{record(1), record(2)};
  ASSERT_THAT(device.submit(span), HasResult());

  // One submission, its buffers listed in execution order, each with its own commands nested
  // under it, so a capture cannot hide which buffer carried which command.
  EXPECT_THAT(device.serialize(),
              HasSubstr("submit serial=1 commandBufferCount=2\n"
                        "  commandBuffer#0 commandCount=1\n"
                        "    copyTextureToBuffer texture=texture#0 buffer=buffer#0 offsetBytes=0 "
                        "bytesPerRow=256 rowsPerImage=4 copySize=4x4\n"
                        "  commandBuffer#1 commandCount=2\n"
                        "    copyTextureToBuffer texture=texture#0 buffer=buffer#0 offsetBytes=0 "
                        "bytesPerRow=256 rowsPerImage=4 copySize=4x4\n"
                        "    copyTextureToBuffer texture=texture#0 buffer=buffer#0 offsetBytes=0 "
                        "bytesPerRow=256 rowsPerImage=4 copySize=4x4\n"));
}

}  // namespace
}  // namespace donner::gpu
