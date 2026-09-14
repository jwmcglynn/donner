#pragma once
/// @file
/// Reflected DisplacementMap resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/DisplacementMap.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateDisplacementMapArtifact() {
  static_assert(shader.resources.size() == 4);
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Compute);
  static_assert(shader.entryPoints[0].workgroupSize[2] == 1);
  static_assert(shader.resource("sourceTexture") &&
                shader.resource("sourceTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("mapTexture") &&
                shader.resource("mapTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("outputTexture") &&
                shader.resource("outputTexture")->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(shader.resource("outputTexture")->storageFormat == TextureFormat::RGBA32Float);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(DisplacementMapParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(DisplacementMapParams));
  static_assert(shader.matchesMember("params", "scale", offsetof(DisplacementMapParams, scale),
                                     sizeof(DisplacementMapParams::scale), ShaderScalarType::F32));
  static_assert(
      shader.matchesMember("params", "xChannel", offsetof(DisplacementMapParams, xChannel),
                           sizeof(DisplacementMapParams::xChannel), ShaderScalarType::U32));
  static_assert(
      shader.matchesMember("params", "yChannel", offsetof(DisplacementMapParams, yChannel),
                           sizeof(DisplacementMapParams::yChannel), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "padding", offsetof(DisplacementMapParams, padding),
                                     sizeof(DisplacementMapParams::padding),
                                     ShaderScalarType::U32));
  return true;
}
}  // namespace donner::gpu::shader::programs
