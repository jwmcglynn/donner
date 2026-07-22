/// @file
/// Handle lifetime tests: RAII release, explicit destroy, stale references after slot reuse,
/// cross-device handles, moved-from handles, and device-teardown ordering.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::Eq;
using testing::HasSubstr;
using testing::Ne;
using testing::Not;

namespace donner::gpu {
namespace {

class HandleLifetimeTests : public testing::Test {
protected:
  Buffer createBuffer(const char* label) {
    return GetResultOrFail(device_.createBuffer(
        BufferDescriptor{label, 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  }

  Status writeTo(const Buffer& buffer) {
    const std::vector<uint8_t> bytes(4, 0xAB);
    return device_.writeBuffer(buffer, 0, bytes);
  }

  RecordingDevice device_;
};

TEST_F(HandleLifetimeTests, DestroyConsumesHandleAndUseFailsClosed) {
  Buffer buffer = createBuffer("victim");
  EXPECT_THAT(device_.destroyBuffer(std::move(buffer)), IsOk());

  // The consumed handle is null; both use and re-destroy fail closed.
  EXPECT_EQ(buffer, nullptr);  // NOLINT(bugprone-use-after-move): consumed state is the API.
  EXPECT_THAT(writeTo(buffer), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.destroyBuffer(std::move(buffer)), IsGpuError(GpuErrorType::InvalidHandle));
}

TEST_F(HandleLifetimeTests, DroppingHandleReleasesResource) {
  {
    const Buffer buffer = createBuffer("raii");
    EXPECT_THAT(device_.serialize(), Not(HasSubstr("destroy buffer#0")));
  }
  EXPECT_THAT(device_.serialize(), HasSubstr("destroy buffer#0"));
}

TEST_F(HandleLifetimeTests, ExplicitDestroyThenRaiiDoesNotDoubleDestroy) {
  {
    Buffer buffer = createBuffer("once");
    EXPECT_THAT(device_.destroyBuffer(std::move(buffer)), IsOk());
  }

  // Exactly one destroy line: the RAII release of the consumed handle is a no-op.
  const std::string capture = device_.serialize();
  const size_t first = capture.find("destroy buffer#0");
  ASSERT_THAT(first, Ne(std::string::npos));
  EXPECT_THAT(capture.find("destroy buffer#0", first + 1), Eq(std::string::npos));
}

TEST_F(HandleLifetimeTests, MoveAssignReleasesPreviouslyOwnedResource) {
  Buffer target = createBuffer("first");
  Buffer replacement = createBuffer("second");

  target = std::move(replacement);

  // The first buffer (slot 0) was released by the assignment; the second is still alive.
  EXPECT_THAT(device_.serialize(), HasSubstr("destroy buffer#0"));
  EXPECT_THAT(device_.serialize(), Not(HasSubstr("destroy buffer#1")));
  EXPECT_THAT(writeTo(target), IsOk());
}

TEST_F(HandleLifetimeTests, HandleOutlivingDeviceReleasesNothing) {
  auto device = std::make_unique<RecordingDevice>();
  Buffer survivor = GetResultOrFail(device->createBuffer(
      BufferDescriptor{"survivor", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));

  // Destroying the device first must leave the outstanding handle inert: its destructor runs
  // after this line and must not touch the destroyed device (ASAN would catch a violation).
  device.reset();
}

TEST_F(HandleLifetimeTests, StaleReferenceAfterSlotReuseFailsClosed) {
  Buffer first = createBuffer("first");
  const uint32_t firstSlot = first.slotIndex();
  const uint32_t firstGeneration = first.generation();
  const BindGroupLayout layout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "uniforms", {BindGroupLayoutEntry{0, ShaderStage::Vertex, BindingType::UniformBuffer}}}));

  // Capture a non-owning reference, then destroy the buffer and reuse its slot.
  const BufferRef staleRef(first);
  EXPECT_THAT(device_.destroyBuffer(std::move(first)), IsOk());
  const Buffer second = createBuffer("second");
  ASSERT_THAT(second.slotIndex(), Eq(firstSlot));
  ASSERT_THAT(second.generation(), Ne(firstGeneration));

  // The stale reference must not alias the slot's new occupant.
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "stale", layout, {BindGroupEntry{0, BufferBinding{staleRef, 0, 16}}}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("stale")));
  EXPECT_THAT(writeTo(second), IsOk());
}

TEST_F(HandleLifetimeTests, ForeignDeviceHandleFailsClosed) {
  RecordingDevice otherDevice;
  Buffer foreign = GetResultOrFail(otherDevice.createBuffer(
      BufferDescriptor{"foreign", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));

  EXPECT_THAT(writeTo(foreign), IsGpuError(GpuErrorType::DeviceMismatch));

  // destroy on the wrong device reports the mismatch; the consumed handle still releases the
  // resource on its OWNING device (no leak), which the other device's capture records.
  EXPECT_THAT(device_.destroyBuffer(std::move(foreign)), IsGpuError(GpuErrorType::DeviceMismatch));
  EXPECT_THAT(otherDevice.serialize(), HasSubstr("destroy buffer#0"));
  EXPECT_THAT(device_.serialize(), Not(HasSubstr("destroy")));
}

TEST_F(HandleLifetimeTests, MovedFromHandleIsNullAndFailsClosed) {
  Buffer original = createBuffer("moved");
  const Buffer moved = std::move(original);

  EXPECT_EQ(original, nullptr);  // NOLINT(bugprone-use-after-move): moved-from state is the API.
  EXPECT_THAT(writeTo(original), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(writeTo(moved), IsOk());

  // The move transferred ownership: no destroy has been recorded yet.
  EXPECT_THAT(device_.serialize(), Not(HasSubstr("destroy")));
}

TEST_F(HandleLifetimeTests, PassAttachmentOfDestroyedTextureRejects) {
  Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"targetView"}));
  ASSERT_THAT(device_.destroyTexture(std::move(texture)), IsOk());

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
  EXPECT_THAT(
      encoder->beginRenderPass(RenderPassDescriptor{
          "pass", {RenderPassColorAttachment{view, LoadOp::Clear, StoreOp::Store}}}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("texture was destroyed")));
}

TEST_F(HandleLifetimeTests, SampledBindingOfDestroyedTextureRejects) {
  Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "image", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
  const TextureView view =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"imageView"}));
  const BindGroupLayout layout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "texture",
          {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat}}}));
  ASSERT_THAT(device_.destroyTexture(std::move(texture)), IsOk());

  EXPECT_THAT(
      device_.createBindGroup(
          BindGroupDescriptor{"group", layout, {BindGroupEntry{0, TextureViewBinding{view}}}}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("texture was destroyed")));
}

TEST_F(HandleLifetimeTests, StaleViewAfterTextureSlotReuseRejects) {
  Texture original = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "original", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const uint32_t originalSlot = original.slotIndex();
  const uint32_t originalGeneration = original.generation();
  const TextureView staleView =
      GetResultOrFail(device_.createTextureView(original, TextureViewDescriptor{"staleView"}));
  ASSERT_THAT(device_.destroyTexture(std::move(original)), IsOk());

  // The replacement reuses the freed texture slot; the stale view must not alias it.
  const Texture replacement = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "replacement", Extent2d{8, 8}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  ASSERT_THAT(replacement.slotIndex(), Eq(originalSlot));
  ASSERT_THAT(replacement.generation(), Ne(originalGeneration));

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
  EXPECT_THAT(
      encoder->beginRenderPass(RenderPassDescriptor{
          "pass", {RenderPassColorAttachment{staleView, LoadOp::Clear, StoreOp::Store}}}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("texture was destroyed")));
}

TEST_F(HandleLifetimeTests, DestroyIsSharedAcrossResourceTypes) {
  Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  TextureView view =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"view"}));
  Sampler sampler = GetResultOrFail(device_.createSampler(SamplerDescriptor{"sampler"}));

  EXPECT_THAT(device_.destroyTextureView(std::move(view)), IsOk());
  EXPECT_THAT(device_.destroyTextureView(std::move(view)),  // NOLINT(bugprone-use-after-move)
              IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.destroySampler(std::move(sampler)), IsOk());

  // A view of a destroyed texture cannot be created either.
  const uint32_t textureSlot = texture.slotIndex();
  const uint32_t textureGeneration = texture.generation();
  const uint64_t deviceId = texture.deviceId();
  EXPECT_THAT(device_.destroyTexture(std::move(texture)), IsOk());
  const Texture staleTexture = Texture::CreateForBackend(textureSlot, textureGeneration, deviceId);
  EXPECT_THAT(device_.createTextureView(staleTexture, TextureViewDescriptor{"lateView"}),
              IsGpuError(GpuErrorType::InvalidHandle));
}

}  // namespace
}  // namespace donner::gpu
