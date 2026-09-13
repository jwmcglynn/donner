/// @file
/// Build-time shader artifacts retain the source module's interface at production call sites.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/tests/GeneratedProgramDescriptorTestCases.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::shader {
namespace {

using tests::kPrograms;
using tests::Program;

class GeneratedProgramDescriptorTests : public testing::TestWithParam<Program> {};

TEST_P(GeneratedProgramDescriptorTests, PreservesSourceAndCompleteInterface) {
  const auto module = GetParam().buildModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto source = EmitWgsl(module.result());
  ASSERT_THAT(source, HasShaderResult());
  const auto bindings = BufferBindingsOf(module.result());
  ASSERT_THAT(bindings, HasShaderResult());
  const auto descriptor = GetParam().buildDescriptor(ShaderSourceKind::Wgsl);
  EXPECT_THAT(descriptor.sourceText, testing::Eq(source.result()));
  ASSERT_THAT(descriptor.bufferBindings, testing::Optional(testing::_));
  EXPECT_THAT(*descriptor.bufferBindings, testing::ElementsAreArray(bindings.result()));
  const auto entries = ComputeEntryPointsOf(module.result());
  ASSERT_THAT(entries, testing::SizeIs(1));
  EXPECT_THAT(
      descriptor.computeEntryPoints,
      testing::ElementsAre(testing::AllOf(
          testing::Field(&ComputeEntryPointInfo::name, entries.front().name),
          testing::Field(&ComputeEntryPointInfo::workgroupSize, entries.front().workgroupSize))));
  if (!GetParam().nativeSources) {
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
