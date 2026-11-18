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
  static_assert(shader.matchesMember("params", "shiftX", offsetof(DropShadowParams, shiftX),
                                     sizeof(DropShadowParams::shiftX), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "shiftY", offsetof(DropShadowParams, shiftY),
                                     sizeof(DropShadowParams::shiftY), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "weightX", offsetof(DropShadowParams, weightX),
                                     sizeof(DropShadowParams::weightX), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "weightY", offsetof(DropShadowParams, weightY),
                                     sizeof(DropShadowParams::weightY), ShaderScalarType::F32));
  return true;
}
}  // namespace donner::gpu::shader::programs
