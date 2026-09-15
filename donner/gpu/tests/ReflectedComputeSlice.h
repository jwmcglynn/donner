#pragma once
/// @file
/// Native compute pipeline construction from a compiled artifact's reflected interface.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "donner/base/RcString.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::tests {

/// Pipeline objects created from one reflected compute artifact.
struct ReflectedComputePipeline {
  ShaderModule module;
  BindGroupLayout layout;
  PipelineLayout pipelineLayout;
  ComputePipeline pipeline;
  WorkgroupSize workgroup{1, 1, 1};  //!< Shape the entry point declares.

  /// Workgroup counts covering \p width by \p height texels.
  std::array<uint32_t, 3> groupsFor(uint32_t width, uint32_t height) const {
    return {(width + workgroup.x - 1) / workgroup.x, (height + workgroup.y - 1) / workgroup.y, 1};
  }
};

/// Reflected group-zero binding slot of \p name; records a test failure when absent.
/// @param shader Static compiled interface. @param name Authored resource name.
inline uint32_t ReflectedBinding(const shader::CompiledShaderView& shader, std::string_view name) {
  const shader::ShaderResource* resource = shader.resource(name);
  EXPECT_NE(resource, nullptr) << "shader declares no resource named " << name;
  return resource != nullptr ? resource->binding : UINT32_MAX;
}

/// Creates the module, group-zero layout and pipeline of \p shader through \p device, binding
/// slots, stage visibility, entry name and workgroup shape all from reflection.
/// @param device Native device. @param shader Selected or mutation artifact.
/// @param label Debug label. @param out Receives the created objects.
template <typename DeviceType>
void CreateReflectedComputePipeline(DeviceType& device, const shader::CompiledShaderView& shader,
                                    const char* label, ReflectedComputePipeline& out) {
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  const shader::ShaderEntryPoint& entry = shader.entryPoints.front();
  ASSERT_EQ(entry.stage, ShaderStage::Compute);
  out.workgroup = {entry.workgroupSize[0], entry.workgroupSize[1], entry.workgroupSize[2]};
  ASSERT_GT(out.workgroup.x, 0u);
  ASSERT_GT(out.workgroup.y, 0u);
  ASSERT_EQ(out.workgroup.z, 1u);
  auto module = device.createShaderModule(
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), label));
  ASSERT_THAT(module, HasResult());
  auto layout = device.createBindGroupLayout(
      BindGroupLayoutDescriptor{label, shader::MakeBindingLayout(shader)});
  ASSERT_THAT(layout, HasResult());
  auto pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{label, {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      label, pipelineLayout.result(), ComputeState{module.result(), RcString(entry.name.view())},
      out.workgroup});
  ASSERT_THAT(pipeline, HasResult());
  out.module = std::move(module).result();
  out.layout = std::move(layout).result();
  out.pipelineLayout = std::move(pipelineLayout).result();
  out.pipeline = std::move(pipeline).result();
}

}  // namespace donner::gpu::tests
