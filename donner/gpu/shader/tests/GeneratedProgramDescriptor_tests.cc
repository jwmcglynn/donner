/// @file
/// Production compute artifacts expose complete descriptors and binding ranges at their call
/// sites.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/tests/GeneratedProgramDescriptorTestCases.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {
namespace {

using tests::kPrograms;
using tests::Program;

class GeneratedProgramDescriptorTests : public testing::TestWithParam<Program> {};

TEST_P(GeneratedProgramDescriptorTests, PreservesSourceAndCompleteInterface) {
  const CompiledShaderView& shader = GetParam().compiledShader();
  const auto descriptor = GetParam().buildDescriptor(ShaderSourceKind::Wgsl);
  EXPECT_THAT(descriptor.sourceText, testing::Eq(shader.wgsl));
  ASSERT_THAT(descriptor.bufferBindings, testing::Optional(testing::_));
  std::vector<const ShaderResource*> buffers;
  for (const auto& resource : shader.resources) {
    if (resource.type == BindingType::UniformBuffer ||
        resource.type == BindingType::ReadOnlyStorageBuffer)
      buffers.push_back(&resource);
  }
  ASSERT_THAT(*descriptor.bufferBindings, testing::SizeIs(buffers.size()));
  for (size_t i = 0; i < buffers.size(); ++i) {
    const auto& binding = descriptor.bufferBindings->at(i);
    EXPECT_EQ(binding.entryPoint, shader.entryPoints.front().name.view());
    EXPECT_EQ(binding.stage, ShaderStage::Compute);
    EXPECT_EQ(binding.group, buffers[i]->group);
    EXPECT_EQ(binding.binding, buffers[i]->binding);
    EXPECT_EQ(binding.type, buffers[i]->type);
    EXPECT_EQ(binding.minSizeBytes, buffers[i]->minSizeBytes);
  }
  ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1));
  EXPECT_THAT(descriptor.computeEntryPoints.front().name,
              testing::Eq(shader.entryPoints.front().name.view()));
  EXPECT_THAT(descriptor.computeEntryPoints.front().workgroupSize,
              testing::Eq((gpu::WorkgroupSize{shader.entryPoints.front().workgroupSize[0],
                                              shader.entryPoints.front().workgroupSize[1],
                                              shader.entryPoints.front().workgroupSize[2]})));
  // The adapter artifact retains only WGSL, so a native descriptor built from it is empty and the
  // device refuses it instead of compiling nothing.
  for (ShaderSourceKind kind : {ShaderSourceKind::Msl, ShaderSourceKind::Spirv}) {
    const auto unavailable = GetParam().buildDescriptor(kind);
    EXPECT_THAT(unavailable.sourceText, testing::IsEmpty());
    EXPECT_THAT(unavailable.spirvWords, testing::IsEmpty());
    EXPECT_THAT(unavailable.bufferBindings, testing::Eq(std::nullopt));
    RecordingDevice device;
    EXPECT_THAT(device.createShaderModule(unavailable),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
}

TEST_P(GeneratedProgramDescriptorTests, ValidatesProductionDescriptorBufferLayoutsAndRanges) {
  const auto descriptor = GetParam().buildDescriptor(ShaderSourceKind::Wgsl);
  ASSERT_THAT(descriptor.bufferBindings, testing::Optional(testing::_));
  ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1));
  const auto& entry = descriptor.computeEntryPoints.front();
  RecordingDevice device;
  const auto module = GetResultOrFail(device.createShaderModule(descriptor));
  std::vector<BindGroupLayoutEntry> entries;
  for (const auto& binding : *descriptor.bufferBindings) {
    ASSERT_THAT(binding.group, testing::Eq(0));
    entries.push_back({binding.binding, binding.stage, binding.type});
  }
  if (entries.empty()) {
    return;
  }
  const auto groupLayout = GetResultOrFail(device.createBindGroupLayout({"valid", entries}));
  const auto layout = GetResultOrFail(device.createPipelineLayout({"valid", {groupLayout}}));
  const auto pipeline = GetResultOrFail(
      device.createComputePipeline({"valid", layout, {module, entry.name}, entry.workgroupSize}));
  for (size_t index = 0; index < entries.size(); ++index) {
    SCOPED_TRACE(testing::Message() << "binding=" << entries[index].binding);
    for (bool wrongType : {false, true}) {
      SCOPED_TRACE(testing::Message() << "wrongType=" << wrongType);
      auto invalid = entries;
      if (wrongType) {
        invalid[index].type = invalid[index].type == BindingType::UniformBuffer
                                  ? BindingType::ReadOnlyStorageBuffer
                                  : BindingType::UniformBuffer;
      } else {
        invalid[index].visibility = ShaderStage::Fragment;
      }
      const auto invalidGroup = GetResultOrFail(device.createBindGroupLayout({"invalid", invalid}));
      const auto invalidLayout =
          GetResultOrFail(device.createPipelineLayout({"invalid", {invalidGroup}}));
      EXPECT_THAT(device.createComputePipeline(
                      {"invalid", invalidLayout, {module, entry.name}, entry.workgroupSize}),
                  IsGpuError(GpuErrorType::InvalidDescriptor));
    }
    for (bool undersized : {false, true}) {
      SCOPED_TRACE(testing::Message() << "undersized=" << undersized);
      std::vector<Buffer> buffers;
      std::vector<BindGroupEntry> resources;
      for (size_t resourceIndex = 0; resourceIndex < entries.size(); ++resourceIndex) {
        const auto& binding = (*descriptor.bufferBindings)[resourceIndex];
        auto buffer = GetResultOrFail(device.createBuffer(
            {"buffer", binding.minSizeBytes,
             binding.type == BindingType::UniformBuffer ? BufferUsage::Uniform
                                                        : BufferUsage::Storage}));
        const uint64_t size = binding.minSizeBytes - (undersized && resourceIndex == index ? 1 : 0);
        resources.push_back({binding.binding, BufferBinding{buffer, 0, size}});
        buffers.push_back(std::move(buffer));
      }
      const auto group =
          GetResultOrFail(device.createBindGroup({"range", groupLayout, std::move(resources)}));
      auto encoder = GetResultOrFail(device.createCommandEncoder());
      auto* pass = GetResultOrFail(encoder->beginComputePass({}));
      ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
      ASSERT_THAT(pass->setBindGroup(0, group), IsOk());
      if (undersized) {
        EXPECT_THAT(pass->dispatchWorkgroups(1), IsGpuError(GpuErrorType::InvalidDescriptor));
        EXPECT_THAT(pass->end(), IsGpuError(GpuErrorType::InvalidDescriptor));
      } else {
        EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());
        EXPECT_THAT(pass->end(), IsOk());
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Generated, GeneratedProgramDescriptorTests, testing::ValuesIn(kPrograms),
                         [](const testing::TestParamInfo<Program>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace donner::gpu::shader
