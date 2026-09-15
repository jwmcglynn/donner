#pragma once
/// @file
/// Reflected UI draw resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/UiDraw.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, both fragment entries and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateUiDrawArtifact() {
  static_assert(shader.resources.size() == 3);
  static_assert(shader.entryPoints.size() == 3);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Vertex);
  static_assert(shader.entryPoints[1].stage == ShaderStage::Fragment);
  static_assert(shader.entryPoints[2].stage == ShaderStage::Fragment);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->binding == 0);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(UiDrawParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(UiDrawParams));
  static_assert(shader.resource("uiSampler") &&
                shader.resource("uiSampler")->type == BindingType::FilteringSampler);
  static_assert(shader.resource("uiSampler")->binding == 1);
  static_assert(shader.resource("uiTexture") &&
                shader.resource("uiTexture")->type == BindingType::SampledTexture2dFloat);
  static_assert(shader.resource("uiTexture")->binding == 2);
  static_assert(shader.matchesMember(
      "params", "clip_from_logical", offsetof(UiDrawParams, clipFromLogical),
      sizeof(UiDrawParams::clipFromLogical), ShaderScalarType::F32, 4, 0, 0, 4, 16));
  return true;
}
}  // namespace donner::gpu::shader::programs
