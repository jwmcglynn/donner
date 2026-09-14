#pragma once
/// @file
/// SVG offset parameters and precompiled shader projections.

#include <cstdint>

#include "donner/gpu/shader/CompiledShader.h"

namespace donner::gpu::shader::programs {

/// Uniform layout for finite pixel shifts in the admitted [-4096, 4096] range.
struct OffsetParams {
  float dx;       //!< Shift along x, rounded half away from zero by the shader.
  float dy;       //!< Shift along y, rounded half away from zero by the shader.
  uint32_t pad0;  //!< Padding to the 16-byte uniform block size.
  uint32_t pad1;  //!< Padding to the 16-byte uniform block size.
};
static_assert(sizeof(OffsetParams) == 16);

/// Returns the authored WGSL offset shader and its reflected resource/compute interface.
/// Source texels outside the input extent become transparent black. Invocations outside the
/// destination extent do not write. Callers provide finite shifts in [-4096, 4096].
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& OffsetShader();

/// Returns only the platform's native MSL or SPIR-V projection and the reflected interface.
/// @return Stable view into a process-lifetime artifact.
const CompiledShaderView& OffsetNativeShader();

}  // namespace donner::gpu::shader::programs
