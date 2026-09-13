/// @file
/// Build-time shader artifacts retain the source module's interface at production call sites.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/generated/ColorSpaceConvertShader.h"
#include "donner/gpu/shader/generated/ComponentTransferShader.h"
#include "donner/gpu/shader/generated/CompositeShader.h"
#include "donner/gpu/shader/generated/ConvolveMatrixShader.h"
#include "donner/gpu/shader/generated/DiffuseLightingShader.h"
#include "donner/gpu/shader/generated/DisplacementMapShader.h"
#include "donner/gpu/shader/generated/DropShadowShader.h"
#include "donner/gpu/shader/generated/FilterColorMatrixShader.h"
#include "donner/gpu/shader/generated/FilterImageShader.h"
#include "donner/gpu/shader/generated/FilterResolveShader.h"
#include "donner/gpu/shader/generated/FloodShader.h"
#include "donner/gpu/shader/generated/GaussianBlurShader.h"
#include "donner/gpu/shader/generated/MergeShader.h"
#include "donner/gpu/shader/generated/MorphologyShader.h"
#include "donner/gpu/shader/generated/OffsetShader.h"
#include "donner/gpu/shader/generated/SnapshotUnpremultiplyShader.h"
#include "donner/gpu/shader/generated/SpecularLightingShader.h"
#include "donner/gpu/shader/generated/SubregionClipShader.h"
#include "donner/gpu/shader/generated/TileShader.h"
#include "donner/gpu/shader/generated/TurbulenceShader.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/programs/ConvolveMatrix.h"
#include "donner/gpu/shader/programs/DisplacementMap.h"
#include "donner/gpu/shader/programs/DropShadow.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/programs/FilterImage.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/programs/Lighting.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/programs/Turbulence.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/benchmarks/AllocationTracker.h"

namespace donner::gpu::shader {
namespace {

struct Program {
  const char* name;
  ShaderResult<IrModule> (*buildModule)();
  ShaderModuleDescriptor (*buildDescriptor)(ShaderSourceKind);
  bool nativeSources;
};

const Program kPrograms[] = {
    {"snapshot_unpremultiply", programs::BuildSnapshotUnpremultiplyModule,
     generated::snapshot_unpremultiply::BuildDescriptor, false},
    {"flood", programs::BuildFloodModule, generated::flood::BuildDescriptor, false},
    {"subregion_clip", programs::BuildSubregionClipModule,
     generated::subregion_clip::BuildDescriptor, false},
    {"offset", programs::BuildOffsetModule, generated::offset::BuildDescriptor, false},
    {"color_space_convert", programs::BuildColorSpaceConvertModule,
     generated::color_space_convert::BuildDescriptor, false},
    {"filter_color_matrix", programs::BuildFilterColorMatrixModule,
     generated::filter_color_matrix::BuildDescriptor, false},
    {"gaussian_blur", programs::BuildGaussianBlurModule, generated::gaussian_blur::BuildDescriptor,
     false},
    {"merge", programs::BuildMergeModule, generated::merge::BuildDescriptor, false},
    {"composite", programs::BuildCompositeModule, generated::composite::BuildDescriptor, false},
    {"morphology", programs::BuildMorphologyModule, generated::morphology::BuildDescriptor, false},
    {"tile", programs::BuildTileModule, generated::tile::BuildDescriptor, false},
    {"filter_resolve", programs::BuildFilterResolveModule,
     generated::filter_resolve::BuildDescriptor, false},
    {"component_transfer", programs::BuildComponentTransferModule,
     generated::component_transfer::BuildDescriptor, true},
    {"displacement_map", programs::BuildDisplacementMapModule,
     generated::displacement_map::BuildDescriptor, true},
    {"drop_shadow", programs::BuildDropShadowModule, generated::drop_shadow::BuildDescriptor, true},
    {"convolve_matrix", programs::BuildConvolveMatrixModule,
     generated::convolve_matrix::BuildDescriptor, true},
    {"filter_image", programs::BuildFilterImageModule, generated::filter_image::BuildDescriptor,
     true},
    {"turbulence", programs::BuildTurbulenceModule, generated::turbulence::BuildDescriptor, true},
    {"diffuse_lighting", programs::BuildDiffuseLightingModule,
     generated::diffuse_lighting::BuildDescriptor, true},
    {"specular_lighting", programs::BuildSpecularLightingModule,
     generated::specular_lighting::BuildDescriptor, true},
};

void PrintTo(const Program& program, std::ostream* stream) {
  *stream << program.name;
}

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

TEST_P(GeneratedProgramDescriptorTests, ReportsPerPipelineMetadataValidationCost) {
  const auto descriptor = GetParam().buildDescriptor(ShaderSourceKind::Wgsl);
  ASSERT_THAT(descriptor.bufferBindings, testing::Optional(testing::_));
  ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1));
  std::vector<BindGroupLayoutEntry> entries;
  for (const auto& binding : *descriptor.bufferBindings) {
    ASSERT_THAT(binding.group, testing::Eq(0));
    entries.push_back({binding.binding, binding.stage, binding.type});
  }
  for (bool metadata : {false, true}) {
    RecordingDevice device;
    auto candidate = descriptor;
    if (!metadata) {
      candidate.bufferBindings.reset();
    }
    const auto module = GetResultOrFail(device.createShaderModule(candidate));
    BindGroupLayout group;
    PipelineLayoutDescriptor layoutDescriptor{"layout", {}};
    if (!entries.empty()) {
      group = GetResultOrFail(device.createBindGroupLayout({"layout", entries}));
      layoutDescriptor.bindGroupLayouts.push_back(group);
    }
    const auto layout = GetResultOrFail(device.createPipelineLayout(layoutDescriptor));
    const auto& entry = candidate.computeEntryPoints.front();
    const ComputePipelineDescriptor pipelineDescriptor{
        "pipeline", layout, {module, entry.name}, entry.workgroupSize};
    const auto warmup = GetResultOrFail(device.createComputePipeline(pipelineDescriptor));
    constexpr size_t kPipelines = 128;
    benchmarks::allocations::Scope allocations;
    const auto start = std::chrono::steady_clock::now();
    for (size_t index = 0; index < kPipelines; ++index) {
      const auto pipeline = GetResultOrFail(device.createComputePipeline(pipelineDescriptor));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto measured = allocations.stop();
    std::fprintf(stderr,
                 "pipeline_metadata program=%s enabled=%d ns=%.3f allocations=%.3f bytes=%.3f\n",
                 GetParam().name, metadata,
                 std::chrono::duration<double, std::nano>(elapsed).count() / kPipelines,
                 double(measured.allocationCalls) / kPipelines,
                 double(measured.allocationBytes) / kPipelines);
  }
}

INSTANTIATE_TEST_SUITE_P(Generated, GeneratedProgramDescriptorTests, testing::ValuesIn(kPrograms),
                         [](const testing::TestParamInfo<Program>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace donner::gpu::shader
