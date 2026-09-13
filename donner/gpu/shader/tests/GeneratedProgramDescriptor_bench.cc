/// @file
/// Explicit allocation measurements, isolated from normal and sanitizer test selections.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/shader/tests/GeneratedProgramDescriptorTestCases.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/benchmarks/AllocationTracker.h"

namespace donner::gpu::shader {
namespace {

using tests::kPrograms;
using tests::Program;

class GeneratedProgramDescriptorBenchmark : public testing::TestWithParam<Program> {};

TEST_P(GeneratedProgramDescriptorBenchmark, ReportsPerPipelineMetadataValidationCost) {
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

INSTANTIATE_TEST_SUITE_P(Generated, GeneratedProgramDescriptorBenchmark,
                         testing::ValuesIn(kPrograms),
                         [](const testing::TestParamInfo<Program>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace donner::gpu::shader
