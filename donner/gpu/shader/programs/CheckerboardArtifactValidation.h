#pragma once
/// @file
/// Reflected Checkerboard resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/Checkerboard.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateCheckerboardArtifact() {
  static_assert(shader.resources.size() == 1);
  static_assert(shader.entryPoints.size() == 2);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Vertex);
  static_assert(shader.entryPoints[1].stage == ShaderStage::Fragment);
  static_assert(shader.resource("params") &&
                shader.resource("params")->type == BindingType::UniformBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(CheckerboardParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(CheckerboardParams));
  static_assert(
      shader.matchesMember("params", "target_size", offsetof(CheckerboardParams, targetSize),
                           sizeof(CheckerboardParams::targetSize), ShaderScalarType::F32, 2));
  static_assert(shader.matchesMember(
      "params", "device_pixel_ratio", offsetof(CheckerboardParams, devicePixelRatio),
      sizeof(CheckerboardParams::devicePixelRatio), ShaderScalarType::F32));
  static_assert(
      shader.matchesMember("params", "checker_size", offsetof(CheckerboardParams, checkerSize),
                           sizeof(CheckerboardParams::checkerSize), ShaderScalarType::F32));
  static_assert(
      shader.matchesMember("params", "dark_color", offsetof(CheckerboardParams, darkColor),
                           sizeof(CheckerboardParams::darkColor), ShaderScalarType::F32, 4));
  static_assert(
      shader.matchesMember("params", "light_color", offsetof(CheckerboardParams, lightColor),
                           sizeof(CheckerboardParams::lightColor), ShaderScalarType::F32, 4));
  static_assert(
      shader.matchesMember("params", "origin_offset", offsetof(CheckerboardParams, originOffsetPx),
                           sizeof(CheckerboardParams::originOffsetPx), ShaderScalarType::F32, 2));
  static_assert(shader.matchesMember("params", "padding", offsetof(CheckerboardParams, padding),
                                     sizeof(CheckerboardParams::padding), ShaderScalarType::F32,
                                     2));
  return true;
}
}  // namespace donner::gpu::shader::programs
