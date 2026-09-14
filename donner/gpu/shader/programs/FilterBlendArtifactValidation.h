#pragma once
/// @file
/// Reflected feBlend resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/FilterBlend.h"
namespace donner::gpu::shader::programs {
/// Verifies all four resources, parameter fields and the compute entry.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateFilterBlendArtifact() {
  static_assert(shader.resources.size() == 4);
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Compute);
  static_assert(shader.entryPoints[0].workgroupSize[2] == 1);
  static_assert(shader.resource("in1_tex") &&
                shader.resource("in1_tex")->type == BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("in2_tex") &&
                shader.resource("in2_tex")->type == BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("output_tex") &&
                shader.resource("output_tex")->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(shader.resource("output_tex")->storageFormat == TextureFormat::RGBA32Float);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(FilterBlendParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(FilterBlendParams));
  static_assert(shader.matchesMember("params", "mode", offsetof(FilterBlendParams, mode),
                                     sizeof(FilterBlendParams::mode), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "_pad0", offsetof(FilterBlendParams, pad0),
                                     sizeof(FilterBlendParams::pad0), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "_pad1", offsetof(FilterBlendParams, pad1),
                                     sizeof(FilterBlendParams::pad1), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "_pad2", offsetof(FilterBlendParams, pad2),
                                     sizeof(FilterBlendParams::pad2), ShaderScalarType::U32));
  return true;
}
}  // namespace donner::gpu::shader::programs
