#pragma once
/// @file
/// Compile-time host-layout checks shared by offset projections.

#include <cstddef>

#include "donner/gpu/shader/programs/Offset.h"

namespace donner::gpu::shader::programs {

/// Checks the exact admitted uniform and resource types while allowing reflected binding changes.
/// @tparam shader Static reflected shader interface.
template <const CompiledShaderView& shader>
consteval bool ValidateOffsetArtifact() {
  constexpr const ShaderResource* params = shader.resource("params");
  constexpr const ShaderResource* input = shader.resource("inputTexture");
  constexpr const ShaderResource* output = shader.resource("outputTexture");
  static_assert(params != nullptr && input != nullptr && output != nullptr);
  static_assert(shader.resources.size() == 3);
  static_assert(params->type == BindingType::UniformBuffer);
  static_assert(input->type == BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(output->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(output->storageFormat == TextureFormat::RGBA32Float);
  static_assert(params->minSizeBytes == sizeof(OffsetParams));
  static_assert(params->alignmentBytes == alignof(OffsetParams));
  static_assert(shader.entryPoints.size() == 1 &&
                shader.entryPoints.front().stage == ShaderStage::Compute);
  static_assert(shader.entryPoints.front().workgroupSize[2] == 1);
  static_assert(shader.matchesMember("params", "shiftX", offsetof(OffsetParams, shiftX),
                                     sizeof(OffsetParams::shiftX), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "shiftY", offsetof(OffsetParams, shiftY),
                                     sizeof(OffsetParams::shiftY), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "weightX", offsetof(OffsetParams, weightX),
                                     sizeof(OffsetParams::weightX), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "weightY", offsetof(OffsetParams, weightY),
                                     sizeof(OffsetParams::weightY), ShaderScalarType::F32));
  return true;
}
}  // namespace donner::gpu::shader::programs
