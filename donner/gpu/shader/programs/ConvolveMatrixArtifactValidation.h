#pragma once
/// @file
/// Compile-time host-layout checks shared by convolution projections.

#include "donner/gpu/shader/programs/ConvolveMatrix.h"

namespace donner::gpu::shader::programs {

/// Checks every source field and resource against the admitted host interface.
/// @tparam shader Static reflected shader interface.
template <const CompiledShaderView& shader>
consteval bool ValidateConvolveMatrixArtifact() {
  constexpr const ShaderResource* params = shader.resource("params");
  constexpr const ShaderResource* input = shader.resource("inputTexture");
  constexpr const ShaderResource* output = shader.resource("outputTexture");
  static_assert(params != nullptr);
  static_assert(input != nullptr);
  static_assert(output != nullptr);
  static_assert(params->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(input->type == BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(output->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(params->minSizeBytes == sizeof(ConvolveMatrixParams));
  static_assert(params->alignmentBytes == alignof(ConvolveMatrixParams));
  static_assert(
      shader.entryPoints.size() == 1 && shader.entryPoints.front().stage == ShaderStage::Compute,
      "The filter artifact requires exactly one compute entry");
  static_assert(shader.entryPoints.front().workgroupSize[2] == 1);
  static_assert(shader.matchesMember("params", "orderX", offsetof(ConvolveMatrixParams, orderX),
                                     sizeof(ConvolveMatrixParams::orderX), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "orderY", offsetof(ConvolveMatrixParams, orderY),
                                     sizeof(ConvolveMatrixParams::orderY), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "targetX", offsetof(ConvolveMatrixParams, targetX),
                                     sizeof(ConvolveMatrixParams::targetX), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "targetY", offsetof(ConvolveMatrixParams, targetY),
                                     sizeof(ConvolveMatrixParams::targetY), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "divisor", offsetof(ConvolveMatrixParams, divisor),
                                     sizeof(ConvolveMatrixParams::divisor), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "bias", offsetof(ConvolveMatrixParams, bias),
                                     sizeof(ConvolveMatrixParams::bias), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "edgeMode", offsetof(ConvolveMatrixParams, edgeMode),
                                     sizeof(ConvolveMatrixParams::edgeMode),
                                     ShaderScalarType::U32));
  static_assert(
      shader.matchesMember("params", "preserveAlpha", offsetof(ConvolveMatrixParams, preserveAlpha),
                           sizeof(ConvolveMatrixParams::preserveAlpha), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "coefficients",
                                     offsetof(ConvolveMatrixParams, kernel),
                                     sizeof(ConvolveMatrixParams::kernel), ShaderScalarType::F32, 1,
                                     kConvolveMatrixKernelCapacity, sizeof(float)));
  return true;
}

}  // namespace donner::gpu::shader::programs
