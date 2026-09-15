#pragma once
/// @file
/// Compile-time host-layout checks shared by the diffuse and specular lighting projections.

#include <cstddef>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/LightingParams.h"

namespace donner::gpu::shader::programs {

/// Checks the exact admitted storage and resource types while allowing reflected binding changes.
/// @tparam shader Static reflected shader interface.
template <const CompiledShaderView& shader>
consteval bool ValidateLightingArtifact() {
  constexpr const ShaderResource* params = shader.resource("params");
  constexpr const ShaderResource* input = shader.resource("inputTexture");
  constexpr const ShaderResource* output = shader.resource("outputTexture");
  static_assert(params != nullptr && input != nullptr && output != nullptr);
  static_assert(shader.resources.size() == 3);
  static_assert(params->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(input->type == BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(output->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(output->storageFormat == TextureFormat::RGBA32Float);
  static_assert(params->minSizeBytes == sizeof(LightingParams));
  static_assert(params->alignmentBytes == alignof(LightingParams));
  static_assert(shader.entryPoints.size() == 1 &&
                shader.entryPoints.front().stage == ShaderStage::Compute);
  static_assert(shader.entryPoints.front().workgroupSize[2] == 1);
  static_assert(shader.matchesMember("params", "surfaceScale",
                                     offsetof(LightingParams, surfaceScale),
                                     sizeof(LightingParams::surfaceScale), ShaderScalarType::F32));
  static_assert(
      shader.matchesMember("params", "lightingConstant", offsetof(LightingParams, lightingConstant),
                           sizeof(LightingParams::lightingConstant), ShaderScalarType::F32));
  static_assert(
      shader.matchesMember("params", "specularExponent", offsetof(LightingParams, specularExponent),
                           sizeof(LightingParams::specularExponent), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pad0", offsetof(LightingParams, pad0),
                                     sizeof(LightingParams::pad0), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "lightR", offsetof(LightingParams, lightR),
                                     sizeof(LightingParams::lightR), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "lightG", offsetof(LightingParams, lightG),
                                     sizeof(LightingParams::lightG), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "lightB", offsetof(LightingParams, lightB),
                                     sizeof(LightingParams::lightB), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "lightType", offsetof(LightingParams, lightType),
                                     sizeof(LightingParams::lightType), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "azimuthRad", offsetof(LightingParams, azimuthRad),
                                     sizeof(LightingParams::azimuthRad), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "elevationRad",
                                     offsetof(LightingParams, elevationRad),
                                     sizeof(LightingParams::elevationRad), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "lightX", offsetof(LightingParams, lightX),
                                     sizeof(LightingParams::lightX), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "lightY", offsetof(LightingParams, lightY),
                                     sizeof(LightingParams::lightY), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "lightZ", offsetof(LightingParams, lightZ),
                                     sizeof(LightingParams::lightZ), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userLightX", offsetof(LightingParams, userLightX),
                                     sizeof(LightingParams::userLightX), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userLightY", offsetof(LightingParams, userLightY),
                                     sizeof(LightingParams::userLightY), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userLightZ", offsetof(LightingParams, userLightZ),
                                     sizeof(LightingParams::userLightZ), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pointsAtX", offsetof(LightingParams, pointsAtX),
                                     sizeof(LightingParams::pointsAtX), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pointsAtY", offsetof(LightingParams, pointsAtY),
                                     sizeof(LightingParams::pointsAtY), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pointsAtZ", offsetof(LightingParams, pointsAtZ),
                                     sizeof(LightingParams::pointsAtZ), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "spotExponent",
                                     offsetof(LightingParams, spotExponent),
                                     sizeof(LightingParams::spotExponent), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userPointsAtX",
                                     offsetof(LightingParams, userPointsAtX),
                                     sizeof(LightingParams::userPointsAtX), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userPointsAtY",
                                     offsetof(LightingParams, userPointsAtY),
                                     sizeof(LightingParams::userPointsAtY), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "userPointsAtZ",
                                     offsetof(LightingParams, userPointsAtZ),
                                     sizeof(LightingParams::userPointsAtZ), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "coneAngleRad",
                                     offsetof(LightingParams, coneAngleRad),
                                     sizeof(LightingParams::coneAngleRad), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pixelToUser0",
                                     offsetof(LightingParams, pixelToUser0),
                                     sizeof(LightingParams::pixelToUser0), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pixelToUser1",
                                     offsetof(LightingParams, pixelToUser1),
                                     sizeof(LightingParams::pixelToUser1), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pixelToUser2",
                                     offsetof(LightingParams, pixelToUser2),
                                     sizeof(LightingParams::pixelToUser2), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pixelToUser3",
                                     offsetof(LightingParams, pixelToUser3),
                                     sizeof(LightingParams::pixelToUser3), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pixelToUser4",
                                     offsetof(LightingParams, pixelToUser4),
                                     sizeof(LightingParams::pixelToUser4), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "pixelToUser5",
                                     offsetof(LightingParams, pixelToUser5),
                                     sizeof(LightingParams::pixelToUser5), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "hasShear", offsetof(LightingParams, hasShear),
                                     sizeof(LightingParams::hasShear), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "hasConeAngle",
                                     offsetof(LightingParams, hasConeAngle),
                                     sizeof(LightingParams::hasConeAngle), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "sampleMinX", offsetof(LightingParams, sampleMinX),
                                     sizeof(LightingParams::sampleMinX), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "sampleMinY", offsetof(LightingParams, sampleMinY),
                                     sizeof(LightingParams::sampleMinY), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "sampleMaxX", offsetof(LightingParams, sampleMaxX),
                                     sizeof(LightingParams::sampleMaxX), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "sampleMaxY", offsetof(LightingParams, sampleMaxY),
                                     sizeof(LightingParams::sampleMaxY), ShaderScalarType::I32));
  return true;
}
}  // namespace donner::gpu::shader::programs
