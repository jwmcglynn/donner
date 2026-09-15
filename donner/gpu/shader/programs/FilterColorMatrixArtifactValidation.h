#pragma once
/// @file
/// Reflected FilterColorMatrix resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/FilterColorMatrix.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateFilterColorMatrixArtifact() {
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
  static_assert(shader.resource("params")->minSizeBytes == sizeof(FilterColorMatrixParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(FilterColorMatrixParams));
  static_assert(shader.matchesMember("params", "col0", offsetof(FilterColorMatrixParams, col0),
                                     sizeof(FilterColorMatrixParams::col0), ShaderScalarType::F32,
                                     4));
  static_assert(shader.matchesMember("params", "col1", offsetof(FilterColorMatrixParams, col1),
                                     sizeof(FilterColorMatrixParams::col1), ShaderScalarType::F32,
                                     4));
  static_assert(shader.matchesMember("params", "col2", offsetof(FilterColorMatrixParams, col2),
                                     sizeof(FilterColorMatrixParams::col2), ShaderScalarType::F32,
                                     4));
  static_assert(shader.matchesMember("params", "col3", offsetof(FilterColorMatrixParams, col3),
                                     sizeof(FilterColorMatrixParams::col3), ShaderScalarType::F32,
                                     4));
  static_assert(shader.matchesMember("params", "col4", offsetof(FilterColorMatrixParams, col4),
                                     sizeof(FilterColorMatrixParams::col4), ShaderScalarType::F32,
                                     4));
  return true;
}
}  // namespace donner::gpu::shader::programs
