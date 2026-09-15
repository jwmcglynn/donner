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
  static_assert(shader.matchesMember("params", "dx", offsetof(OffsetParams, dx),
                                     sizeof(OffsetParams::dx), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "dy", offsetof(OffsetParams, dy),
                                     sizeof(OffsetParams::dy), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pad0", offsetof(OffsetParams, pad0),
                                     sizeof(OffsetParams::pad0), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad1", offsetof(OffsetParams, pad1),
                                     sizeof(OffsetParams::pad1), ShaderScalarType::U32));
  return true;
}
}  // namespace donner::gpu::shader::programs
