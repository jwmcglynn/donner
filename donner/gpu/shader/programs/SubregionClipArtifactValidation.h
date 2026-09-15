#pragma once
/// @file
/// Reflected SubregionClip resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/SubregionClip.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateSubregionClipArtifact() {
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
  static_assert(shader.resource("params")->minSizeBytes == sizeof(SubregionClipParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(SubregionClipParams));
  static_assert(shader.matchesMember("params", "invA", offsetof(SubregionClipParams, invA),
                                     sizeof(SubregionClipParams::invA), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invB", offsetof(SubregionClipParams, invB),
                                     sizeof(SubregionClipParams::invB), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invC", offsetof(SubregionClipParams, invC),
                                     sizeof(SubregionClipParams::invC), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invD", offsetof(SubregionClipParams, invD),
                                     sizeof(SubregionClipParams::invD), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invE", offsetof(SubregionClipParams, invE),
                                     sizeof(SubregionClipParams::invE), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invF", offsetof(SubregionClipParams, invF),
                                     sizeof(SubregionClipParams::invF), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userX0", offsetof(SubregionClipParams, userX0),
                                     sizeof(SubregionClipParams::userX0), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userY0", offsetof(SubregionClipParams, userY0),
                                     sizeof(SubregionClipParams::userY0), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userX1", offsetof(SubregionClipParams, userX1),
                                     sizeof(SubregionClipParams::userX1), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userY1", offsetof(SubregionClipParams, userY1),
                                     sizeof(SubregionClipParams::userY1), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pad0", offsetof(SubregionClipParams, pad0),
                                     sizeof(SubregionClipParams::pad0), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad1", offsetof(SubregionClipParams, pad1),
                                     sizeof(SubregionClipParams::pad1), ShaderScalarType::U32));
  return true;
}
}  // namespace donner::gpu::shader::programs
