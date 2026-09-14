#pragma once
/// @file
/// Compile-time resolve resource and host-layout verification.
#include <cstddef>

#include "donner/gpu/shader/programs/FilterResolve.h"
namespace donner::gpu::shader::programs {
/// Checks the admitted resource kinds, format, uniform fields and transfer-table layout.
/// @tparam shader Static shader interface.
template <const CompiledShaderView& shader>
consteval bool ValidateFilterResolveArtifact() {
  constexpr auto* input = shader.resource("inputTexture");
  constexpr auto* output = shader.resource("outputTexture");
  constexpr auto* params = shader.resource("params");
  constexpr auto* table = shader.resource("transferTable");
  static_assert(input && output && params && table);
  static_assert(shader.resources.size() == 4);
  static_assert(input->type == BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(output->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(output->storageFormat == TextureFormat::RGBA8Unorm);
  static_assert(params->type == BindingType::UniformBuffer);
  static_assert(params->minSizeBytes == sizeof(FilterResolveParams));
  static_assert(params->alignmentBytes == alignof(FilterResolveParams));
  static_assert(table->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(table->minSizeBytes == kFilterResolveTransferCount * sizeof(float));
  static_assert(shader.matchesMember(
      "transferTable", "samples", 0, kFilterResolveTransferCount * sizeof(float),
      ShaderScalarType::F32, 1, kFilterResolveTransferCount, sizeof(float)));
  static_assert(shader.entryPoints.size() == 1 &&
                shader.entryPoints.front().stage == ShaderStage::Compute);
  static_assert(shader.entryPoints.front().workgroupSize[2] == 1);
  static_assert(shader.matchesMember("params", "invA", offsetof(FilterResolveParams, invA),
                                     sizeof(FilterResolveParams::invA), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invB", offsetof(FilterResolveParams, invB),
                                     sizeof(FilterResolveParams::invB), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invC", offsetof(FilterResolveParams, invC),
                                     sizeof(FilterResolveParams::invC), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invD", offsetof(FilterResolveParams, invD),
                                     sizeof(FilterResolveParams::invD), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invE", offsetof(FilterResolveParams, invE),
                                     sizeof(FilterResolveParams::invE), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "invF", offsetof(FilterResolveParams, invF),
                                     sizeof(FilterResolveParams::invF), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userX0", offsetof(FilterResolveParams, userX0),
                                     sizeof(FilterResolveParams::userX0), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userY0", offsetof(FilterResolveParams, userY0),
                                     sizeof(FilterResolveParams::userY0), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userX1", offsetof(FilterResolveParams, userX1),
                                     sizeof(FilterResolveParams::userX1), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userY1", offsetof(FilterResolveParams, userY1),
                                     sizeof(FilterResolveParams::userY1), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pad0", offsetof(FilterResolveParams, pad0),
                                     sizeof(FilterResolveParams::pad0), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad1", offsetof(FilterResolveParams, pad1),
                                     sizeof(FilterResolveParams::pad1), ShaderScalarType::U32));
  return true;
}
}  // namespace donner::gpu::shader::programs
