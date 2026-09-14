#pragma once
/// @file
/// Reflected Morphology resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/Morphology.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateMorphologyArtifact() {
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
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(MorphologyParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(MorphologyParams));
  static_assert(shader.matchesMember("params", "radiusX", offsetof(MorphologyParams, radiusX),
                                     sizeof(MorphologyParams::radiusX), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "radiusY", offsetof(MorphologyParams, radiusY),
                                     sizeof(MorphologyParams::radiusY), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "op", offsetof(MorphologyParams, op),
                                     sizeof(MorphologyParams::op), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad", offsetof(MorphologyParams, pad),
                                     sizeof(MorphologyParams::pad), ShaderScalarType::U32));
  return true;
}
}  // namespace donner::gpu::shader::programs
