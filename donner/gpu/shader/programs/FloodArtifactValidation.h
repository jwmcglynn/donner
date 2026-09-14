#pragma once
/// @file
/// Reflected Flood resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/Flood.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateFloodArtifact() {
  static_assert(shader.resources.size() == 2);
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Compute);
  static_assert(shader.entryPoints[0].workgroupSize[2] == 1);
  static_assert(shader.resource("outputTexture") &&
                shader.resource("outputTexture")->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(shader.resource("outputTexture")->storageFormat == TextureFormat::RGBA32Float);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(FloodParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(FloodParams));
  static_assert(shader.matchesMember("params", "color", offsetof(FloodParams, color),
                                     sizeof(FloodParams::color), ShaderScalarType::F32, 4));
  return true;
}
}  // namespace donner::gpu::shader::programs
