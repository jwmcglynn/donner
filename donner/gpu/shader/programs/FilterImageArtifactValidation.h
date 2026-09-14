#pragma once
/// @file
/// Reflected FilterImage resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/FilterImage.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateFilterImageArtifact() {
  static_assert(shader.resources.size() == 3);
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Compute);
  static_assert(shader.entryPoints[0].workgroupSize[2] == 1);
  static_assert(shader.resource("imageTexture") &&
                shader.resource("imageTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("outputTexture") &&
                shader.resource("outputTexture")->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(shader.resource("outputTexture")->storageFormat == TextureFormat::RGBA32Float);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(FilterImageParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(FilterImageParams));
  static_assert(shader.matchesMember("params", "m00", offsetof(FilterImageParams, m00),
                                     sizeof(FilterImageParams::m00), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "m01", offsetof(FilterImageParams, m01),
                                     sizeof(FilterImageParams::m01), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "m02", offsetof(FilterImageParams, m02),
                                     sizeof(FilterImageParams::m02), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "m10", offsetof(FilterImageParams, m10),
                                     sizeof(FilterImageParams::m10), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "m11", offsetof(FilterImageParams, m11),
                                     sizeof(FilterImageParams::m11), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "m12", offsetof(FilterImageParams, m12),
                                     sizeof(FilterImageParams::m12), ShaderScalarType::F32));
  static_assert(
      shader.matchesMember("params", "samplingMode", offsetof(FilterImageParams, samplingMode),
                           sizeof(FilterImageParams::samplingMode), ShaderScalarType::U32));
  static_assert(shader.matchesMember(
      "params", "pixelatedScaleX", offsetof(FilterImageParams, pixelatedScaleX),
      sizeof(FilterImageParams::pixelatedScaleX), ShaderScalarType::F32));
  static_assert(shader.matchesMember(
      "params", "pixelatedScaleY", offsetof(FilterImageParams, pixelatedScaleY),
      sizeof(FilterImageParams::pixelatedScaleY), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "padding", offsetof(FilterImageParams, padding),
                                     sizeof(FilterImageParams::padding), ShaderScalarType::U32));
  return true;
}
}  // namespace donner::gpu::shader::programs
