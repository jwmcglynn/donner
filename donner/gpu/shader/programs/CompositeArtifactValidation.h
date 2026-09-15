#pragma once
/// @file
/// Reflected Composite resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/Composite.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateCompositeArtifact() {
  static_assert(shader.resources.size() == 4);
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Compute);
  static_assert(shader.entryPoints[0].workgroupSize[2] == 1);
  static_assert(shader.resource("sourceTexture") &&
                shader.resource("sourceTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("destinationTexture") &&
                shader.resource("destinationTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("outputTexture") &&
                shader.resource("outputTexture")->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(shader.resource("outputTexture")->storageFormat == TextureFormat::RGBA32Float);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(CompositeParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(CompositeParams));
  static_assert(shader.matchesMember("params", "op", offsetof(CompositeParams, op),
                                     sizeof(CompositeParams::op), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad0", offsetof(CompositeParams, pad0),
                                     sizeof(CompositeParams::pad0), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad1", offsetof(CompositeParams, pad1),
                                     sizeof(CompositeParams::pad1), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad2", offsetof(CompositeParams, pad2),
                                     sizeof(CompositeParams::pad2), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "k1", offsetof(CompositeParams, k1),
                                     sizeof(CompositeParams::k1), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "k2", offsetof(CompositeParams, k2),
                                     sizeof(CompositeParams::k2), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "k3", offsetof(CompositeParams, k3),
                                     sizeof(CompositeParams::k3), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "k4", offsetof(CompositeParams, k4),
                                     sizeof(CompositeParams::k4), ShaderScalarType::F32));
  return true;
}
}  // namespace donner::gpu::shader::programs
