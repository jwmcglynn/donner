#pragma once
/// @file
/// Reflected ComponentTransfer resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/ComponentTransfer.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateComponentTransferArtifact() {
  static_assert(shader.resources.size() == 3);
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Compute);
  static_assert(shader.entryPoints[0].workgroupSize[2] == 1);
  static_assert(shader.resource("inputTexture") &&
                shader.resource("inputTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("outputTexture") &&
                shader.resource("outputTexture")->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(shader.resource("outputTexture")->storageFormat == TextureFormat::RGBA32Float);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("params")->runtimeArrayStrideBytes == sizeof(float));
  static_assert(shader.resource("params")->minSizeBytes == sizeof(float));
  static_assert(shader.resource("params")->runtimeArrayScalarType == ShaderScalarType::F32);
  static_assert(shader.resource("params")->runtimeArrayLanes == 1);
  return true;
}
}  // namespace donner::gpu::shader::programs
