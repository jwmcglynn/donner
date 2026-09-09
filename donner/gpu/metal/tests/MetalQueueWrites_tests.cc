/// @file
/// Queue writes preserve earlier submissions while later submissions observe
/// the new contents.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal::tests {
namespace {

struct Capture {
  Buffer readback;
  uint64_t serial = 0;
  Extent2d size{1, 1};
};

class MetalQueueWritesTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "Metal queue write ordering");
  }

  void TearDown() override {
    if (device_) {
      device_->resumeSubmissionsForTest();
    }
  }

  Capture captureTexture(const Texture& texture, Extent2d size = {1, 1}) {
    Capture capture;
    capture.size = size;
    capture.readback = GetResultOrFail(device_->createBuffer(BufferDescriptor{
        "capture", uint64_t{256} * size.height, BufferUsage::CopyDst | BufferUsage::MapRead}));
    auto encoder = GetResultOrFail(device_->createCommandEncoder());
    EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{texture}, capture.readback,
                                             TexelCopyBufferLayout{0, 256, size.height}, size),
                IsOk());
    capture.serial = GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
    return capture;
  }

  Capture captureUniform(const Buffer& uniform) {
    const ShaderModule module = GetResultOrFail(device_->createShaderModule(
        ShaderModuleDescriptor{"captureUniform",
                               RcString(R"msl(#include <metal_stdlib>
using namespace metal;
kernel void cs_main(constant float4& color [[buffer(1)]],
                    texture2d<float, access::write> output [[texture(1)]]) {
  output.write(color, uint2(0));
}
)msl"),
                               ShaderSourceKind::Msl,
                               {},
                               {ComputeEntryPointInfo{"cs_main", WorkgroupSize{1, 1, 1}}}}));
    const BindGroupLayout layout =
        GetResultOrFail(device_->createBindGroupLayout(BindGroupLayoutDescriptor{
            "captureLayout",
            {{0, ShaderStage::Compute, BindingType::UniformBuffer},
             {1, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d}}}));
    const PipelineLayout pipelineLayout = GetResultOrFail(
        device_->createPipelineLayout(PipelineLayoutDescriptor{"capturePipelineLayout", {layout}}));
    const ComputePipeline pipeline = GetResultOrFail(device_->createComputePipeline(
        ComputePipelineDescriptor{"capturePipeline", pipelineLayout,
                                  ComputeState{module, "cs_main"}, WorkgroupSize{1, 1, 1}}));
    const Texture output = GetResultOrFail(device_->createTexture(
        TextureDescriptor{"captureOutput", Extent2d{1, 1}, TextureFormat::RGBA8Unorm,
                          TextureUsage::StorageBinding | TextureUsage::CopySrc}));
    const TextureView view =
        GetResultOrFail(device_->createTextureView(output, TextureViewDescriptor{"captureView"}));
    const BindGroup group = GetResultOrFail(device_->createBindGroup(
        BindGroupDescriptor{"captureGroup",
                            layout,
                            {{0, BufferBinding{uniform, 0, 16}}, {1, TextureViewBinding{view}}}}));
    Capture capture;
    capture.readback = GetResultOrFail(device_->createBuffer(
        BufferDescriptor{"capture", 256, BufferUsage::CopyDst | BufferUsage::MapRead}));
    auto encoder = GetResultOrFail(device_->createCommandEncoder());
    ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({"capture"}));
    EXPECT_THAT(pass->setPipeline(pipeline), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, group), IsOk());
    EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());
    EXPECT_THAT(pass->end(), IsOk());
    EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{output}, capture.readback,
                                             TexelCopyBufferLayout{0, 256, 1}, Extent2d{1, 1}),
                IsOk());
    capture.serial = GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
    return capture;
  }

  void expectPixels(const Capture& capture, std::span<const uint8_t> expected, const char* label) {
    const auto bytes = GetResultOrFail(device_->readBackBuffer(capture.readback));
    ASSERT_GE(bytes.size(), uint64_t{256} * capture.size.height);
    const Vector2i dimensions{static_cast<int>(capture.size.width),
                              static_cast<int>(capture.size.height)};
    const svg::RendererBitmap actual{.dimensions = dimensions, .pixels = bytes, .rowBytes = 256};
    const size_t rowBytes = size_t{capture.size.width} * 4;
    ASSERT_EQ(expected.size(), rowBytes * capture.size.height);
    svg::RendererBitmap reference{
        .dimensions = dimensions, .pixels = std::vector<uint8_t>(bytes.size()), .rowBytes = 256};
    for (uint32_t row = 0; row < capture.size.height; ++row) {
      std::copy_n(expected.begin() + row * rowBytes, rowBytes,
                  reference.pixels.begin() + row * 256);
    }
    editor::tests::CompareBitmapToBitmap(actual, reference, label,
                                         editor::tests::PixelmatchIdentityParams());
  }

  void expectPixel(const Capture& capture, std::array<uint8_t, 4> expected, const char* label) {
    expectPixels(capture, expected, label);
  }

  Status writeColor(const Buffer& buffer, const std::array<float, 4>& color) {
    return device_->writeBuffer(
        buffer, 0,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(color.data()), sizeof(color)));
  }

  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalQueueWritesTest, UniformUpdateDoesNotChangeAnEarlierSubmission) {
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  const std::array<float, 4> red = {1.0f, 0.0f, 0.0f, 1.0f};
  const std::array<float, 4> blue = {0.0f, 0.0f, 1.0f, 1.0f};
  ASSERT_THAT(
      device_->writeBuffer(
          uniform, 0, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(red.data()), 16)),
      IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  ASSERT_THAT(device_->completedSerial(), testing::Lt(first.serial));
  ASSERT_THAT(
      device_->writeBuffer(
          uniform, 0, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(blue.data()), 16)),
      IsOk());
  const Capture second = captureUniform(uniform);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "uniform_before_update");
  expectPixel(second, {0, 0, 255, 255}, "uniform_after_update");
}

TEST_F(MetalQueueWritesTest, TextureUpdateDoesNotChangeAnEarlierSubmission) {
  const Texture texture = GetResultOrFail(
      device_->createTexture(TextureDescriptor{"source", Extent2d{1, 1}, TextureFormat::RGBA8Unorm,
                                               TextureUsage::CopySrc | TextureUsage::CopyDst}));
  const std::array<uint8_t, 4> red = {255, 0, 0, 255};
  const std::array<uint8_t, 4> blue = {0, 0, 255, 255};
  const TexelCopyBufferLayout layout{0, 256, 1};
  ASSERT_THAT(device_->writeTexture(texture, red, layout, Extent2d{1, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureTexture(texture);
  ASSERT_THAT(device_->completedSerial(), testing::Lt(first.serial));
  ASSERT_THAT(device_->writeTexture(texture, blue, layout, Extent2d{1, 1}), IsOk());
  const Capture second = captureTexture(texture);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, red, "texture_before_update");
  expectPixel(second, blue, "texture_after_update");
}

TEST_F(MetalQueueWritesTest, RepeatedWritesCoalesceAndOverlapsKeepQueueOrder) {
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  for (int i = 0; i < 32; ++i) {
    ASSERT_THAT(writeColor(uniform, {0, 0, 1, 1}), IsOk());
  }
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 1u);
  const float redChannel = 1.0f;
  ASSERT_THAT(
      device_->writeBuffer(uniform, 0,
                           std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&redChannel),
                                                    sizeof(redChannel))),
      IsOk());
  const Capture second = captureUniform(uniform);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "coalesced_before_update");
  expectPixel(second, {255, 0, 255, 255}, "coalesced_after_update");
  const MetalDevice::WriteStats stats = device_->writeStatsForTest();
  EXPECT_EQ(stats.pendingWrites, 0u);
  EXPECT_EQ(stats.pendingStagingBytes, 0u);
  EXPECT_EQ(stats.stagingAllocations, 1u);
  EXPECT_EQ(stats.submittedUploadBatches, 1u);
  EXPECT_EQ(stats.unalignedWriteWaits, 0u);
  EXPECT_EQ(device_->lastSubmittedSerial(), 2u);
}

TEST_F(MetalQueueWritesTest, PartialTextureUploadsPreserveOtherPixelsAndCallerPitch) {
  const Texture texture = GetResultOrFail(
      device_->createTexture(TextureDescriptor{"source", Extent2d{2, 2}, TextureFormat::RGBA8Unorm,
                                               TextureUsage::CopySrc | TextureUsage::CopyDst}));
  std::array<uint8_t, 512> initial{};
  for (size_t offset : {0u, 4u, 256u, 260u}) {
    initial[offset] = 255;
    initial[offset + 3] = 255;
  }
  ASSERT_THAT(device_->writeTexture(texture, initial, TexelCopyBufferLayout{0, 256, 2}, {2, 2}),
              IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureTexture(texture, {2, 2});
  std::array<uint8_t, 1024> update{};
  for (size_t offset : {4u, 516u}) {
    update[offset + 2] = 255;
    update[offset + 3] = 255;
  }
  ASSERT_THAT(device_->writeTexture(texture, update, TexelCopyBufferLayout{4, 512, 2}, {1, 2}),
              IsOk());
  const Capture second = captureTexture(texture, {2, 2});
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  const std::array<uint8_t, 16> red = {255, 0, 0, 255, 255, 0, 0, 255,
                                       255, 0, 0, 255, 255, 0, 0, 255};
  const std::array<uint8_t, 16> columns = {0, 0, 255, 255, 255, 0, 0, 255,
                                           0, 0, 255, 255, 255, 0, 0, 255};
  expectPixels(first, red, "partial_texture_before_update");
  expectPixels(second, columns, "partial_texture_after_update");
}

TEST_F(MetalQueueWritesTest, StagedFloatTextureUploadsPreserveAllChannelsAndCallerPitch) {
  const Texture texture = GetResultOrFail(device_->createTexture(TextureDescriptor{
      "float", {2, 2}, TextureFormat::RGBA32Float, TextureUsage::CopySrc | TextureUsage::CopyDst}));
  const std::array<float, 4> initialColor{0.125f, 0.25f, 0.5f, 0.75f};
  const std::array<float, 4> updateColor{0.25f, 0.375f, 0.625f, 0.875f};
  std::array<uint8_t, 512> initial{};
  for (size_t offset : {0u, 16u, 256u, 272u}) {
    std::memcpy(initial.data() + offset, initialColor.data(), sizeof(initialColor));
  }
  ASSERT_THAT(device_->writeTexture(texture, initial, {0, 256, 2}, {2, 2}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureTexture(texture, {2, 2});
  std::array<uint8_t, 1024> update{};
  for (size_t offset : {16u, 528u}) {
    std::memcpy(update.data() + offset, updateColor.data(), sizeof(updateColor));
  }
  ASSERT_THAT(device_->writeTexture(texture, update, {16, 512, 2}, {1, 2}), IsOk());
  const Capture second = captureTexture(texture, {2, 2});
  device_->resumeSubmissionsForTest();
  ASSERT_THAT(device_->waitForSerial(second.serial, 5.0), testing::IsTrue());
  const auto beforeBytes = device_->readBackBuffer(first.readback);
  const auto afterBytes = device_->readBackBuffer(second.readback);
  ASSERT_THAT(beforeBytes, HasResult());
  ASSERT_THAT(afterBytes, HasResult());
  ASSERT_THAT(beforeBytes.result(), testing::SizeIs(testing::Ge(288u)));
  ASSERT_THAT(afterBytes.result(), testing::SizeIs(testing::Ge(288u)));
  std::array<float, 16> before{};
  std::array<float, 16> after{};
  for (size_t row = 0; row < 2; ++row) {
    std::memcpy(before.data() + row * 8, beforeBytes.result().data() + row * 256, 32);
    std::memcpy(after.data() + row * 8, afterBytes.result().data() + row * 256, 32);
  }
  EXPECT_THAT(before, testing::ElementsAre(0.125f, 0.25f, 0.5f, 0.75f, 0.125f, 0.25f, 0.5f, 0.75f,
                                           0.125f, 0.25f, 0.5f, 0.75f, 0.125f, 0.25f, 0.5f, 0.75f));
  EXPECT_THAT(after,
              testing::ElementsAre(0.25f, 0.375f, 0.625f, 0.875f, 0.125f, 0.25f, 0.5f, 0.75f, 0.25f,
                                   0.375f, 0.625f, 0.875f, 0.125f, 0.25f, 0.5f, 0.75f));
}

TEST_F(MetalQueueWritesTest, AnUploadOnlySubmissionStillProtectsItsDestination) {
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  ASSERT_THAT(writeColor(uniform, {0, 0, 1, 1}), IsOk());
  device_->resumeSubmissionsForTest();
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  auto empty = GetResultOrFail(device_->createCommandEncoder());
  const uint64_t uploadSerial = GetResultOrFail(device_->submit(GetResultOrFail(empty->finish())));
  ASSERT_TRUE(device_->waitForSerial(first.serial, 5.0)) << device_->lastErrorForTest();
  ASSERT_THAT(device_->completedSerial(), testing::Lt(uploadSerial));
  ASSERT_THAT(writeColor(uniform, {0, 1, 0, 1}), IsOk());
  const Capture third = captureUniform(uniform);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(third.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "upload_only_before_update");
  expectPixel(third, {0, 255, 0, 255}, "upload_only_after_update");
  EXPECT_EQ(device_->writeStatsForTest().submittedUploadBatches, 2u);
}

TEST_F(MetalQueueWritesTest, RetiringADestinationDiscardsItsUnsubmittedWrite) {
  Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  ASSERT_THAT(writeColor(uniform, {0, 0, 1, 1}), IsOk());
  ASSERT_THAT(device_->destroyBuffer(std::move(uniform)), IsOk());
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 0u);
  EXPECT_EQ(device_->writeStatsForTest().pendingStagingBytes, 0u);
  auto empty = GetResultOrFail(device_->createCommandEncoder());
  const uint64_t unrelatedSerial =
      GetResultOrFail(device_->submit(GetResultOrFail(empty->finish())));
  EXPECT_EQ(device_->writeStatsForTest().stagingAllocations, 0u);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(unrelatedSerial, 5.0)) << device_->lastErrorForTest();
  device_->poll();
  expectPixel(first, {255, 0, 0, 255}, "retired_destination");
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 0u);
  EXPECT_EQ(device_->writeStatsForTest().pendingStagingBytes, 0u);
  EXPECT_EQ(device_->writeStatsForTest().stagingAllocations, 0u);
}

TEST_F(MetalQueueWritesTest, ManagedMemoryPublishesTheBatchedUpload) {
  device_ = MetalDevice::Create(MetalDevice::MemoryModel::ForceNonUnified);
  ASSERT_NE(device_, nullptr);
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  ASSERT_THAT(writeColor(uniform, {0, 0, 1, 1}), IsOk());
  const Capture second = captureUniform(uniform);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "managed_before_update");
  expectPixel(second, {0, 0, 255, 255}, "managed_after_update");
  EXPECT_EQ(device_->writeStatsForTest().stagingAllocations, 1u);
  EXPECT_EQ(device_->hostWritePublishCountForTest(), 2u);
}

TEST_F(MetalQueueWritesTest, ManagedUploadOnlySubmissionPublishesItsHostBuffer) {
  device_ = MetalDevice::Create(MetalDevice::MemoryModel::ForceNonUnified);
  ASSERT_NE(device_, nullptr);
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  const std::array<float, 4> blue = {0, 0, 1, 1};
  ASSERT_THAT(writeColor(uniform, blue), IsOk());
  auto empty = GetResultOrFail(device_->createCommandEncoder());
  const uint64_t serial = GetResultOrFail(device_->submit(GetResultOrFail(empty->finish())));
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "managed_upload_only_before_update");
  EXPECT_THAT(
      GetResultOrFail(device_->readBackBuffer(uniform)),
      testing::ElementsAreArray(reinterpret_cast<const uint8_t*>(blue.data()), sizeof(blue)));
  EXPECT_EQ(device_->deviceWritePublishCountForTest(), 2u);
}

TEST_F(MetalQueueWritesTest, UploadBudgetRefusesAndRecoversAcrossInFlightSubmissions) {
  device_ = MetalDevice::Create(MetalDevice::MemoryModel::Detected, 16);
  ASSERT_NE(device_, nullptr);
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  ASSERT_THAT(writeColor(uniform, {0, 0, 1, 1}), IsOk());
  const Capture second = captureUniform(uniform);
  EXPECT_EQ(device_->writeStatsForTest().inFlightStagingBytes, 256u);
  EXPECT_THAT(writeColor(uniform, {0, 1, 0, 1}), IsGpuError(GpuErrorType::LimitExceeded));
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 0u);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  EXPECT_EQ(device_->writeStatsForTest().inFlightStagingBytes, 0u);
  expectPixel(first, {255, 0, 0, 255}, "budget_first_submission");
  expectPixel(second, {0, 0, 255, 255}, "budget_refused_write");

  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture third = captureUniform(uniform);
  ASSERT_THAT(writeColor(uniform, {0, 1, 0, 1}), IsOk());
  const Capture fourth = captureUniform(uniform);
  EXPECT_EQ(device_->writeStatsForTest().inFlightStagingBytes, 256u);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(fourth.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(third, {0, 0, 255, 255}, "budget_recovery_before_update");
  expectPixel(fourth, {0, 255, 0, 255}, "budget_recovery_after_update");
  EXPECT_EQ(device_->writeStatsForTest().inFlightStagingBytes, 0u);
  EXPECT_EQ(device_->writeStatsForTest().stagingAllocations, 2u);
}

TEST_F(MetalQueueWritesTest, RaiiRetirementDiscardsABusyBuffersUnsubmittedWrite) {
  Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  ASSERT_THAT(writeColor(uniform, {0, 0, 1, 1}), IsOk());
  uniform = Buffer{};
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 0u);
  EXPECT_EQ(device_->writeStatsForTest().pendingStagingBytes, 0u);
  auto empty = GetResultOrFail(device_->createCommandEncoder());
  const uint64_t serial = GetResultOrFail(device_->submit(GetResultOrFail(empty->finish())));
  EXPECT_EQ(device_->writeStatsForTest().stagingAllocations, 0u);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "raii_retired_buffer");
}

TEST_F(MetalQueueWritesTest, TextureRetirementDiscardsWritesBeforeTheEarlierSubmissionCompletes) {
  for (bool explicitDestroy : {true, false}) {
    SCOPED_TRACE(explicitDestroy);
    Texture texture =
        GetResultOrFail(device_->createTexture({"source",
                                                {1, 1},
                                                TextureFormat::RGBA8Unorm,
                                                TextureUsage::CopySrc | TextureUsage::CopyDst}));
    const std::array<uint8_t, 4> red = {255, 0, 0, 255};
    const std::array<uint8_t, 4> blue = {0, 0, 255, 255};
    ASSERT_THAT(device_->writeTexture(texture, red, {0, 256, 1}, {1, 1}), IsOk());
    ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
    const Capture first = captureTexture(texture);
    ASSERT_THAT(device_->writeTexture(texture, blue, {0, 256, 1}, {1, 1}), IsOk());
    if (explicitDestroy) {
      ASSERT_THAT(device_->destroyTexture(std::move(texture)), IsOk());
    } else {
      texture = Texture{};
    }
    EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 0u);
    EXPECT_EQ(device_->writeStatsForTest().pendingStagingBytes, 0u);
    auto empty = GetResultOrFail(device_->createCommandEncoder());
    const uint64_t serial = GetResultOrFail(device_->submit(GetResultOrFail(empty->finish())));
    EXPECT_EQ(device_->writeStatsForTest().stagingAllocations, 0u);
    device_->resumeSubmissionsForTest();
    ASSERT_TRUE(device_->waitForSerial(serial, 5.0)) << device_->lastErrorForTest();
    expectPixel(first, red, explicitDestroy ? "explicit_retired_texture" : "raii_retired_texture");
  }
}

TEST_F(MetalQueueWritesTest, SmallPayloadsShareTheLogicalBudgetDespiteStagingAlignment) {
  device_ = MetalDevice::Create(MetalDevice::MemoryModel::Detected, 16);
  ASSERT_NE(device_, nullptr);
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 32, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  const std::array<float, 2> zeros = {0, 0};
  const std::array<float, 2> ones = {1, 1};
  const std::array<float, 2> redGreen = {1, 0};
  const auto bytes = [](const auto& value) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), sizeof(value));
  };
  ASSERT_THAT(device_->writeBuffer(uniform, 0, bytes(zeros)), IsOk());
  ASSERT_THAT(device_->writeBuffer(uniform, 8, bytes(ones)), IsOk());
  ASSERT_THAT(device_->writeBuffer(uniform, 0, bytes(redGreen)), IsOk());
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 2u);
  EXPECT_EQ(device_->writeStatsForTest().pendingStagingBytes, 512u);
  EXPECT_THAT(device_->writeBuffer(uniform, 16, bytes(zeros)),
              IsGpuError(GpuErrorType::LimitExceeded));
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 2u);
  EXPECT_EQ(device_->writeStatsForTest().pendingStagingBytes, 512u);
  const Capture second = captureUniform(uniform);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "logical_budget_before_update");
  expectPixel(second, {255, 0, 255, 255}, "logical_budget_coalesced_ranges");
  EXPECT_EQ(device_->writeStatsForTest().stagingAllocations, 1u);
}

TEST_F(MetalQueueWritesTest, TextureBudgetCountsTexelsInsteadOfCallerOrStagingRowPadding) {
  device_ = MetalDevice::Create(MetalDevice::MemoryModel::Detected, 8);
  ASSERT_NE(device_, nullptr);
  const Texture texture =
      GetResultOrFail(device_->createTexture({"source",
                                              {1, 2},
                                              TextureFormat::RGBA8Unorm,
                                              TextureUsage::CopySrc | TextureUsage::CopyDst}));
  std::array<uint8_t, 268> pixels{};
  for (size_t row : {0u, 1u}) {
    pixels[4 + row * 256] = 255;
    pixels[7 + row * 256] = 255;
  }
  ASSERT_THAT(device_->writeTexture(texture, pixels, {4, 256, 2}, {1, 2}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureTexture(texture, {1, 2});
  for (size_t row : {0u, 1u}) {
    pixels[4 + row * 256] = 0;
    pixels[6 + row * 256] = 255;
  }
  ASSERT_THAT(device_->writeTexture(texture, pixels, {4, 256, 2}, {1, 2}), IsOk());
  EXPECT_EQ(device_->writeStatsForTest().pendingStagingBytes, 512u);
  const Capture second = captureTexture(texture, {1, 2});
  EXPECT_EQ(device_->writeStatsForTest().inFlightStagingBytes, 512u);
  EXPECT_THAT(device_->writeTexture(texture, pixels, {4, 256, 2}, {1, 2}),
              IsGpuError(GpuErrorType::LimitExceeded));
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 0u);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  const std::array<uint8_t, 8> red = {255, 0, 0, 255, 255, 0, 0, 255};
  const std::array<uint8_t, 8> blue = {0, 0, 255, 255, 0, 0, 255, 255};
  expectPixels(first, red, "logical_texture_budget_before_update");
  expectPixels(second, blue, "logical_texture_budget_after_update");
  EXPECT_EQ(device_->writeStatsForTest().inFlightStagingBytes, 0u);
}

TEST_F(MetalQueueWritesTest, UnalignedWriteTimesOutWithoutOverwritingTheBusyBuffer) {
  device_ = MetalDevice::Create(MetalDevice::MemoryModel::Detected, kMaxBufferByteSize,
                                std::chrono::milliseconds(20));
  ASSERT_NE(device_, nullptr);
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(writeColor(uniform, {1, 0, 0, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  const std::array<uint8_t, 1> zero = {0};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(device_->writeBuffer(uniform, 3, zero), IsGpuError(GpuErrorType::InvalidState));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::seconds(1));
  RecordProperty("unaligned_wait_ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(first.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "unaligned_timeout_before_update");
  ASSERT_THAT(device_->writeBuffer(uniform, 3, zero), IsOk());
  const Capture second = captureUniform(uniform);
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(second, {0, 0, 0, 255}, "unaligned_after_completion");
  EXPECT_EQ(device_->writeStatsForTest().unalignedWriteWaits, 1u);
}

}  // namespace
}  // namespace donner::gpu::metal::tests
