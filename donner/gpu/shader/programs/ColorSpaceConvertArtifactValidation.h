#pragma once
/// @file
/// Reflected ColorSpaceConvert resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateColorSpaceConvertArtifact() {
  static_assert(shader.resources.size() == 4);
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
  static_assert(shader.resource("transferTable") &&
                shader.resource("transferTable")->type == BindingType::ReadOnlyStorageBuffer);
  static_assert(shader.resource("params")->minSizeBytes == sizeof(ColorSpaceConvertParams));
  static_assert(shader.resource("params")->alignmentBytes == alignof(ColorSpaceConvertParams));
  static_assert(
      shader.matchesMember("params", "direction", offsetof(ColorSpaceConvertParams, direction),
                           sizeof(ColorSpaceConvertParams::direction), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad0", offsetof(ColorSpaceConvertParams, pad0),
                                     sizeof(ColorSpaceConvertParams::pad0), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad1", offsetof(ColorSpaceConvertParams, pad1),
                                     sizeof(ColorSpaceConvertParams::pad1), ShaderScalarType::U32));
  static_assert(shader.matchesMember("params", "pad2", offsetof(ColorSpaceConvertParams, pad2),
                                     sizeof(ColorSpaceConvertParams::pad2), ShaderScalarType::U32));
  static_assert(shader.resource("transferTable")->minSizeBytes == sizeof(ColorTransferTable));
  static_assert(shader.resource("transferTable")->alignmentBytes == alignof(ColorTransferTable));
  static_assert(shader.matchesMember("transferTable", "samples",
                                     offsetof(ColorTransferTable, samples),
                                     sizeof(ColorTransferTable::samples), ShaderScalarType::F32, 1,
                                     2 * kColorTransferSampleCount, sizeof(float)));
  return true;
}
}  // namespace donner::gpu::shader::programs
