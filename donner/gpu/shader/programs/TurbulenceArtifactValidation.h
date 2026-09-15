#pragma once
/// @file
/// Compile-time verification of turbulence storage and compute reflection.
#include <cstddef>

#include "donner/gpu/shader/programs/Turbulence.h"
namespace donner::gpu::shader::programs {
/// Checks every shared parameter and table field, resource format and compute surface.
/// @tparam shader Static reflected shader interface.
template <const CompiledShaderView& shader>
consteval bool ValidateTurbulenceArtifact() {
  constexpr auto* output = shader.resource("outputTexture");
  constexpr auto* params = shader.resource("params");
  constexpr auto* tables = shader.resource("tables");
  static_assert(output && params && tables && shader.resources.size() == 3);
  static_assert(output->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(output->storageFormat == TextureFormat::RGBA32Float);
  static_assert(params->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(tables->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(params->minSizeBytes == sizeof(TurbulenceParams));
  static_assert(params->alignmentBytes == alignof(TurbulenceParams));
  static_assert(tables->minSizeBytes == sizeof(TurbulenceTables));
  static_assert(tables->alignmentBytes == alignof(TurbulenceTables));
  static_assert(shader.entryPoints.size() == 1 &&
                shader.entryPoints.front().stage == ShaderStage::Compute);
  static_assert(shader.entryPoints.front().workgroupSize[2] == 1);
  static_assert(shader.matchesMember("params", "baseFreqX", offsetof(TurbulenceParams, baseFreqX),
                                     sizeof(TurbulenceParams::baseFreqX), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "baseFreqY", offsetof(TurbulenceParams, baseFreqY),
                                     sizeof(TurbulenceParams::baseFreqY), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "numOctaves", offsetof(TurbulenceParams, numOctaves),
                                     sizeof(TurbulenceParams::numOctaves), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "seed", offsetof(TurbulenceParams, seed),
                                     sizeof(TurbulenceParams::seed), ShaderScalarType::I32));
  static_assert(shader.matchesMember("params", "stitchTiles",
                                     offsetof(TurbulenceParams, stitchTiles),
                                     sizeof(TurbulenceParams::stitchTiles), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "typeFlag", offsetof(TurbulenceParams, typeFlag),
                                     sizeof(TurbulenceParams::typeFlag), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "tileWidth", offsetof(TurbulenceParams, tileWidth),
                                     sizeof(TurbulenceParams::tileWidth), ShaderScalarType::F32));
  static_assert(shader.matchesMember("params", "tileHeight", offsetof(TurbulenceParams, tileHeight),
                                     sizeof(TurbulenceParams::tileHeight), ShaderScalarType::F32));
  static_assert(shader.matchesMember(
      "params", "filterFromDeviceA", offsetof(TurbulenceParams, filterFromDeviceA),
      sizeof(TurbulenceParams::filterFromDeviceA), ShaderScalarType::F32));
  static_assert(shader.matchesMember(
      "params", "filterFromDeviceB", offsetof(TurbulenceParams, filterFromDeviceB),
      sizeof(TurbulenceParams::filterFromDeviceB), ShaderScalarType::F32));
  static_assert(shader.matchesMember(
      "params", "filterFromDeviceC", offsetof(TurbulenceParams, filterFromDeviceC),
      sizeof(TurbulenceParams::filterFromDeviceC), ShaderScalarType::F32));
  static_assert(shader.matchesMember(
      "params", "filterFromDeviceD", offsetof(TurbulenceParams, filterFromDeviceD),
      sizeof(TurbulenceParams::filterFromDeviceD), ShaderScalarType::F32));
  static_assert(shader.matchesMember("tables", "lattice", offsetof(TurbulenceTables, lattice),
                                     sizeof(TurbulenceTables::lattice), ShaderScalarType::I32, 1,
                                     kTurbulenceTableSize, 4));
  static_assert(shader.matchesMember("tables", "gradX", offsetof(TurbulenceTables, gradX),
                                     sizeof(TurbulenceTables::gradX), ShaderScalarType::F32, 1,
                                     kTurbulenceGradientTableSize, 4));
  static_assert(shader.matchesMember("tables", "gradY", offsetof(TurbulenceTables, gradY),
                                     sizeof(TurbulenceTables::gradY), ShaderScalarType::F32, 1,
                                     kTurbulenceGradientTableSize, 4));
  return true;
}
}  // namespace donner::gpu::shader::programs
