#pragma once
/// @file
/// Shared feBlend host parameters and selected frozen shader projections.
#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"
namespace donner::gpu::shader::programs {
/// Parameters for the sixteen SVG blend modes.
struct FilterBlendParams {
  uint32_t mode;  //!< SVG blend-mode index from zero through fifteen.
  uint32_t pad0;  //!< Reserved layout padding.
  uint32_t pad1;  //!< Reserved layout padding.
  uint32_t pad2;  //!< Reserved layout padding.
};
static_assert(sizeof(FilterBlendParams) == 16);
/// Returns the WGSL feBlend artifact.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& FilterBlendShader();
/// Returns only the platform-native feBlend projection.
/// @return Stable view into process-lifetime data.
const CompiledShaderView& FilterBlendNativeShader();
}  // namespace donner::gpu::shader::programs
