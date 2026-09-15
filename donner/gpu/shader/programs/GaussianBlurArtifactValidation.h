#pragma once
/// @file
/// Shared compile-time host-layout checks for Gaussian blur artifacts.

#include <cstddef>

#include "donner/gpu/shader/programs/GaussianBlur.h"

namespace donner::gpu::shader::programs {

/// Returns whether \p shader reflects the host Gaussian blur layout.
/// @tparam shader Static reflected shader interface.
template <const CompiledShaderView& shader>
consteval bool ValidateGaussianBlurArtifact() {
  constexpr const ShaderResource* params = shader.resource("params");
  constexpr const ShaderResource* input = shader.resource("inputTexture");
  constexpr const ShaderResource* output = shader.resource("outputTexture");
  static_assert(input);
  static_assert(input->type == BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(output);
  static_assert(output->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(params);
  static_assert(params->type == BindingType::UniformBuffer);
  static_assert(params->minSizeBytes == sizeof(GaussianBlurParams));
  static_assert(params->alignmentBytes == alignof(GaussianBlurParams));
  static_assert(
      shader.entryPoints.size() == 1 && shader.entryPoints.front().stage == ShaderStage::Compute,
      "The filter artifact requires exactly one compute entry");
  static_assert(shader.entryPoints.front().workgroupSize[2] == 1);
  static_assert(
      shader.matchesMember("params", "stdDeviation", offsetof(GaussianBlurParams, stdDeviation),
                           sizeof(GaussianBlurParams::stdDeviation), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "axis", offsetof(GaussianBlurParams, axis),
                                     sizeof(GaussianBlurParams::axis), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "edgeMode", offsetof(GaussianBlurParams, edgeMode),
                                     sizeof(GaussianBlurParams::edgeMode), ShaderScalarType::U32));
  static_assert(
      shader.matchesMember("params", "kernelType", offsetof(GaussianBlurParams, kernelType),
                           sizeof(GaussianBlurParams::kernelType), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "boxLeft", offsetof(GaussianBlurParams, boxLeft),
                                     sizeof(GaussianBlurParams::boxLeft), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "boxRight", offsetof(GaussianBlurParams, boxRight),
                                     sizeof(GaussianBlurParams::boxRight), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "clipMin", offsetof(GaussianBlurParams, clipMin),
                                     sizeof(GaussianBlurParams::clipMin), ShaderScalarType::I32,
                                     2));
  static_assert(shader.matchesMember("params", "clipMax", offsetof(GaussianBlurParams, clipMax),
                                     sizeof(GaussianBlurParams::clipMax), ShaderScalarType::I32,
                                     2));
  static_assert(
      shader.matchesMember("params", "clipActive", offsetof(GaussianBlurParams, clipActive),
                           sizeof(GaussianBlurParams::clipActive), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad", offsetof(GaussianBlurParams, pad),
                                     sizeof(GaussianBlurParams::pad), ShaderScalarType::U32));
  return true;
}

}  // namespace donner::gpu::shader::programs
