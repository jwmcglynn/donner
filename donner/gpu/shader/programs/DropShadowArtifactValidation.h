#pragma once
/// @file
/// Reflected DropShadow resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/DropShadow.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateDropShadowArtifact() {
  static_assert(shader.resources.size() == 4);
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Compute);
  static_assert(shader.entryPoints[0].workgroupSize[2] == 1);
  static_assert(shader.resource("sourceTexture") &&
                shader.resource("sourceTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("blurredTexture") &&
                shader.resource("blurredTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("outputTexture") &&
                shader.resource("outputTexture")->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(shader.resource("outputTexture")->storageFormat == TextureFormat::RGBA32Float);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(DropShadowParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(DropShadowParams));
  static_assert(shader.matchesMember("params", "color", offsetof(DropShadowParams, color),
                                     sizeof(DropShadowParams::color), ShaderScalarType::F32, 4));
  static_assert(shader.matchesMember("params", "dx", offsetof(DropShadowParams, dx),
                                     sizeof(DropShadowParams::dx), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "dy", offsetof(DropShadowParams, dy),
                                     sizeof(DropShadowParams::dy), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pad0", offsetof(DropShadowParams, pad0),
                                     sizeof(DropShadowParams::pad0), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad1", offsetof(DropShadowParams, pad1),
                                     sizeof(DropShadowParams::pad1), ShaderScalarType::U32));
  return true;
}
}  // namespace donner::gpu::shader::programs
