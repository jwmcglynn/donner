/// @file
/// Declared buffer ranges must cover the active shader's generated minimum requirements.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu {
namespace {

using testing::HasSubstr;

class BufferBindingBoundsTests : public testing::Test {
protected:
  void SetUp() override {
    buffer_ = GetResultOrFail(device_.createBuffer({"largeBuffer", 512, BufferUsage::Uniform}));
    groupLayout_ = GetResultOrFail(device_.createBindGroupLayout(
        {"group", {{0, ShaderStage::Compute, BindingType::UniformBuffer}}}));
    layout_ = GetResultOrFail(device_.createPipelineLayout({"layout", {groupLayout_}}));
  }

  ComputePipeline createPipeline(uint64_t minSize) {
    ShaderModuleDescriptor descriptor{"shader",
                                      "@compute @workgroup_size(1) fn cs() {}",
                                      ShaderSourceKind::Wgsl,
                                      {},
                                      {{"cs", {1, 1, 1}}}};
    descriptor.bufferBindings = std::vector<ShaderBufferBindingInfo>{
        {"cs", ShaderStage::Compute, 0, 0, BindingType::UniformBuffer, minSize}};
    modules_.push_back(GetResultOrFail(device_.createShaderModule(descriptor)));
    return GetResultOrFail(
        device_.createComputePipeline({"pipeline", layout_, {modules_.back(), "cs"}, {1, 1, 1}}));
  }

  BindGroup createGroup(uint64_t size, uint64_t offset = 0) {
    return GetResultOrFail(device_.createBindGroup(
        {"group", groupLayout_, {{0, BufferBinding{buffer_, offset, size}}}}));
  }

  RecordingDevice device_;
  Buffer buffer_;
  BindGroupLayout groupLayout_;
  PipelineLayout layout_;
  std::vector<ShaderModule> modules_;
};

TEST_F(BufferBindingBoundsTests, OneByteAndOneByteShortRangesFailBeforeDispatch) {
  const ComputePipeline pipeline = createPipeline(80);
  for (uint64_t size : {uint64_t{1}, uint64_t{79}}) {
    SCOPED_TRACE(size);
    const BindGroup group = createGroup(size);
    auto encoder = GetResultOrFail(device_.createCommandEncoder());
    ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({}));
    ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
    ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
    EXPECT_THAT(pass->dispatchWorkgroups(1),
                IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("80")));
  }
}

TEST_F(BufferBindingBoundsTests, ExactMinimumRangeIsAccepted) {
  const ComputePipeline pipeline = createPipeline(80);
  const BindGroup group = createGroup(80);
  auto encoder = GetResultOrFail(device_.createCommandEncoder());
  ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({}));
  ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());
}

TEST_F(BufferBindingBoundsTests, LargeAllocationDoesNotEnlargeAnOffsetDeclaredRange) {
  const ComputePipeline pipeline = createPipeline(80);
  const BindGroup group = createGroup(79, 256);
  auto encoder = GetResultOrFail(device_.createCommandEncoder());
  ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({}));
  ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
  ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("80")));
}

TEST_F(BufferBindingBoundsTests, PipelineSwitchRechecksAlreadyBoundBufferRanges) {
  const ComputePipeline small = createPipeline(16);
  const ComputePipeline large = createPipeline(80);
  const BindGroup group = createGroup(16);
  auto encoder = GetResultOrFail(device_.createCommandEncoder());
  ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({}));
  ASSERT_THAT(pass->setPipeline(small), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
  ASSERT_THAT(pass->dispatchWorkgroups(1), IsOk());
  ASSERT_THAT(pass->setPipeline(large), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("80")));
}

TEST_F(BufferBindingBoundsTests, PipelineRetainsRequirementsAfterShaderModuleDestruction) {
  const ComputePipeline pipeline = createPipeline(80);
  ASSERT_THAT(device_.destroyShaderModule(std::move(modules_.back())), IsOk());
  const BindGroup group = createGroup(16);
  auto encoder = GetResultOrFail(device_.createCommandEncoder());
  ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({}));
  ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
  EXPECT_THAT(pass->dispatchWorkgroups(1),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("80")));
}

TEST(ShaderBufferMetadataTests, MalformedFactsFailAtModuleCreation) {
  RecordingDevice device;
  const ShaderBufferBindingInfo valid{
      "cs", ShaderStage::Compute, 0, 0, BindingType::ReadOnlyStorageBuffer, 16, 16};
  std::vector<ShaderBufferBindingInfo> invalid(8, valid);
  invalid[0].entryPoint = "";
  invalid[1].stage = ShaderStage::Vertex | ShaderStage::Compute;
  invalid[2].group = kMaxBindGroups;
  invalid[3].binding = kMaxBindings;
  invalid[4].type = BindingType::FilteringSampler;
  invalid[5].minSizeBytes = 0;
  invalid[6].minSizeBytes = kMaxBufferByteSize + 1;
  invalid[7].runtimeArrayStrideBytes = 32;
  for (size_t index = 0; index < invalid.size(); ++index) {
    SCOPED_TRACE(index);
    ShaderModuleDescriptor descriptor{"invalid", "shader", ShaderSourceKind::Wgsl};
    descriptor.bufferBindings = std::vector<ShaderBufferBindingInfo>{invalid[index]};
    EXPECT_THAT(device.createShaderModule(descriptor), IsGpuError(GpuErrorType::InvalidDescriptor));
  }
}

TEST(ShaderBufferMetadataTests, PipelineRejectsMissingBindingTypeAndVisibilityMismatches) {
  RecordingDevice device;
  ShaderModuleDescriptor descriptor{
      "shader", "shader", ShaderSourceKind::Wgsl, {}, {{"cs", {1, 1, 1}}}};
  descriptor.bufferBindings = std::vector<ShaderBufferBindingInfo>{
      {"cs", ShaderStage::Compute, 0, 1, BindingType::UniformBuffer, 80}};
  const ShaderModule module = GetResultOrFail(device.createShaderModule(descriptor));
  const std::array<BindGroupLayoutEntry, 3> entries = {
      BindGroupLayoutEntry{0, ShaderStage::Compute, BindingType::UniformBuffer},
      BindGroupLayoutEntry{1, ShaderStage::Compute, BindingType::ReadOnlyStorageBuffer},
      BindGroupLayoutEntry{1, ShaderStage::Vertex, BindingType::UniformBuffer}};
  for (const auto& entry : entries) {
    SCOPED_TRACE(entry.binding);
    const BindGroupLayout group = GetResultOrFail(device.createBindGroupLayout({"group", {entry}}));
    const PipelineLayout layout = GetResultOrFail(device.createPipelineLayout({"layout", {group}}));
    EXPECT_THAT(device.createComputePipeline({"pipeline", layout, {module, "cs"}, {1, 1, 1}}),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
}

TEST(ShaderBufferMetadataTests, RenderStagesUseTheLargestRequirementForTheirSharedBinding) {
  RecordingDevice device;
  ShaderModuleDescriptor descriptor{"shader", "shader", ShaderSourceKind::Wgsl};
  descriptor.bufferBindings = std::vector<ShaderBufferBindingInfo>{
      {"vs", ShaderStage::Vertex, 0, 0, BindingType::UniformBuffer, 16},
      {"fs", ShaderStage::Fragment, 0, 0, BindingType::UniformBuffer, 80}};
  const ShaderModule module = GetResultOrFail(device.createShaderModule(descriptor));
  const BindGroupLayout groupLayout = GetResultOrFail(device.createBindGroupLayout(
      {"group", {{0, ShaderStage::Vertex | ShaderStage::Fragment, BindingType::UniformBuffer}}}));
  const PipelineLayout layout =
      GetResultOrFail(device.createPipelineLayout({"layout", {groupLayout}}));
  const RenderPipeline pipeline = GetResultOrFail(device.createRenderPipeline(
      {"pipeline", layout, {module, "vs", {}}, {module, "fs", {{TextureFormat::RGBA8Unorm}}}}));
  const Buffer buffer = GetResultOrFail(device.createBuffer({"buffer", 80, BufferUsage::Uniform}));
  const Texture target = GetResultOrFail(device.createTexture(
      {"target", {1, 1}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view = GetResultOrFail(device.createTextureView(target, {}));
  for (uint64_t size : {uint64_t{79}, uint64_t{80}}) {
    SCOPED_TRACE(size);
    const BindGroup group = GetResultOrFail(
        device.createBindGroup({"group", groupLayout, {{0, BufferBinding{buffer, 0, size}}}}));
    auto encoder = GetResultOrFail(device.createCommandEncoder());
    RenderPassEncoder* pass = GetResultOrFail(
        encoder->beginRenderPass({"render", {{view, LoadOp::Clear, StoreOp::Store, {}}}}));
    ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
    ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
    if (size == 80) {
      EXPECT_THAT(pass->draw(3), IsOk());
    } else {
      EXPECT_THAT(pass->draw(3), IsGpuError(GpuErrorType::InvalidDescriptor));
    }
  }
}

std::string RecordShaderFacts(std::optional<std::vector<ShaderBufferBindingInfo>> facts) {
  RecordingDevice device;
  ShaderModuleDescriptor descriptor{"shader", "shader", ShaderSourceKind::Wgsl};
  descriptor.bufferBindings = std::move(facts);
  const ShaderModule module = GetResultOrFail(device.createShaderModule(descriptor));
  return device.serialize();
}

TEST(ShaderBufferRecordingTests, OmittedAndKnownEmptyMetadataAreDifferent) {
  EXPECT_NE(RecordShaderFacts(std::nullopt),
            RecordShaderFacts(std::vector<ShaderBufferBindingInfo>{}));
}

TEST(ShaderBufferRecordingTests, SuppliedFactsAreCompleteAndDeterministic) {
  std::vector<ShaderBufferBindingInfo> facts{
      {"cs", ShaderStage::Compute, 2, 3, BindingType::ReadOnlyStorageBuffer, 16, 16}};
  const std::string recorded = RecordShaderFacts(facts);
  EXPECT_EQ(recorded, RecordShaderFacts(facts));
  EXPECT_THAT(recorded, HasSubstr("bufferBindings=[{entryPoint=\"cs\" stage=Compute group=2 "
                                  "binding=3 type=ReadOnlyStorageBuffer minSizeBytes=16 "
                                  "runtimeArrayStrideBytes=16}]"));
  facts[0].minSizeBytes = 32;
  facts[0].runtimeArrayStrideBytes = 32;
  EXPECT_NE(recorded, RecordShaderFacts(facts));
}

}  // namespace
}  // namespace donner::gpu
