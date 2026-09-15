#pragma once
/// @file
/// Reflected Tile resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/Tile.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateTileArtifact() {
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
  static_assert(shader.resource("params")->minSizeBytes == sizeof(TileParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(TileParams));
  static_assert(shader.matchesMember("params", "srcX", offsetof(TileParams, srcX),
                                     sizeof(TileParams::srcX), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "srcY", offsetof(TileParams, srcY),
                                     sizeof(TileParams::srcY), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "srcW", offsetof(TileParams, srcW),
                                     sizeof(TileParams::srcW), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "srcH", offsetof(TileParams, srcH),
                                     sizeof(TileParams::srcH), ShaderScalarType::I32));
  return true;
}
}  // namespace donner::gpu::shader::programs
