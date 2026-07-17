/// @file
/// Handle lifetime tests: destroy-then-use, stale generations after slot reuse, cross-device
/// handles, and moved-from handles.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::Eq;
using testing::Ne;

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

TEST_F(HandleLifetimeTests, DestroyThenUseFailsClosed) {
  const Buffer buffer = createBuffer("victim");
  EXPECT_THAT(device_.destroyBuffer(buffer), IsOk());

  EXPECT_THAT(writeTo(buffer), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.destroyBuffer(buffer), IsGpuError(GpuErrorType::InvalidHandle));
}

TEST_F(HandleLifetimeTests, StaleGenerationAfterSlotReuseFailsClosed) {
  const Buffer first = createBuffer("first");
  EXPECT_THAT(device_.destroyBuffer(first), IsOk());

  // The freed slot is reused with a bumped generation; the stale handle must not alias it.
  const Buffer second = createBuffer("second");
  ASSERT_THAT(second.slotIndex(), Eq(first.slotIndex()));
  ASSERT_THAT(second.generation(), Ne(first.generation()));

  EXPECT_THAT(writeTo(first), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(writeTo(second), IsOk());
}

TEST_F(HandleLifetimeTests, ForeignDeviceHandleFailsClosed) {
  RecordingDevice otherDevice;
  const Buffer foreign = GetResultOrFail(otherDevice.createBuffer(
      BufferDescriptor{"foreign", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));

  EXPECT_THAT(writeTo(foreign), IsGpuError(GpuErrorType::DeviceMismatch));
  EXPECT_THAT(device_.destroyBuffer(foreign), IsGpuError(GpuErrorType::DeviceMismatch));
}

TEST_F(HandleLifetimeTests, MovedFromHandleIsNullAndFailsClosed) {
  Buffer original = createBuffer("moved");
  const Buffer moved = std::move(original);

  EXPECT_EQ(original, nullptr);  // NOLINT(bugprone-use-after-move): moved-from state is the API.
  EXPECT_THAT(writeTo(original), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(writeTo(moved), IsOk());
}

TEST_F(HandleLifetimeTests, PassAttachmentOfDestroyedTextureRejects) {
  const Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"targetView"}));
  ASSERT_THAT(device_.destroyTexture(texture), IsOk());

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
  EXPECT_THAT(encoder->beginRenderPass(RenderPassDescriptor{
                  "pass", {RenderPassColorAttachment{view, LoadOp::Clear, StoreOp::Store}}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                    testing::HasSubstr("texture was destroyed")));
}

TEST_F(HandleLifetimeTests, SampledBindingOfDestroyedTextureRejects) {
  const Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "image", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
  const TextureView view =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"imageView"}));
  const BindGroupLayout layout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "texture",
          {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat}}}));
  ASSERT_THAT(device_.destroyTexture(texture), IsOk());

  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", layout, {BindGroupEntry{0, TextureViewBinding{view}}}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                    testing::HasSubstr("texture was destroyed")));
}

TEST_F(HandleLifetimeTests, StaleViewAfterTextureSlotReuseRejects) {
  const Texture original = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "original", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView staleView =
      GetResultOrFail(device_.createTextureView(original, TextureViewDescriptor{"staleView"}));
  ASSERT_THAT(device_.destroyTexture(original), IsOk());

  // The replacement reuses the freed texture slot; the stale view must not alias it.
  const Texture replacement = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "replacement", Extent2d{8, 8}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  ASSERT_THAT(replacement.slotIndex(), Eq(original.slotIndex()));
  ASSERT_THAT(replacement.generation(), Ne(original.generation()));

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
  EXPECT_THAT(encoder->beginRenderPass(RenderPassDescriptor{
                  "pass", {RenderPassColorAttachment{staleView, LoadOp::Clear, StoreOp::Store}}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                    testing::HasSubstr("texture was destroyed")));
}

TEST_F(HandleLifetimeTests, DestroyIsSharedAcrossResourceTypes) {
  const Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"view"}));
  const Sampler sampler = GetResultOrFail(device_.createSampler(SamplerDescriptor{"sampler"}));

  EXPECT_THAT(device_.destroyTextureView(view), IsOk());
  EXPECT_THAT(device_.destroyTextureView(view), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.destroyTexture(texture), IsOk());
  EXPECT_THAT(device_.destroySampler(sampler), IsOk());

  // A view of a destroyed texture cannot be created either.
  EXPECT_THAT(device_.createTextureView(texture, TextureViewDescriptor{"lateView"}),
              IsGpuError(GpuErrorType::InvalidHandle));
}

}  // namespace
}  // namespace donner::gpu
