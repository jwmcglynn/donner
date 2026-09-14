#pragma once
/// @file
/// Reflected SnapshotUnpremultiply resource and host-layout checks.
#include <cstddef>

#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"
namespace donner::gpu::shader::programs {
/// Verifies every resource role, the entry interface and the host layout.
/// @tparam shader Static compiled interface.
template <const CompiledShaderView& shader>
consteval bool ValidateSnapshotUnpremultiplyArtifact() {
  static_assert(shader.resources.size() == 2);
  static_assert(shader.entryPoints.size() == 1);
  static_assert(shader.entryPoints[0].stage == ShaderStage::Compute);
  static_assert(shader.entryPoints[0].workgroupSize[2] == 1);
  static_assert(shader.resource("inputTexture") &&
                shader.resource("inputTexture")->type ==
                    BindingType::SampledTexture2dUnfilterableFloat);
  static_assert(shader.resource("outputTexture") &&
                shader.resource("outputTexture")->type == BindingType::WriteOnlyStorageTexture2d);
  static_assert(shader.resource("outputTexture")->storageFormat == TextureFormat::RGBA8Unorm);
  return true;
}
}  // namespace donner::gpu::shader::programs
