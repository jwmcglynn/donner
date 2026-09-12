#pragma once
/// @file
/// Bridges an \c donner::gpu::shader::IrModule to the descriptors the GPU runtime consumes.
///
/// The runtime cannot read a workgroup size back out of compiled WGSL, MSL, or SPIR-V, so a
/// shader module descriptor carries the compute entry points its source declares. Deriving that
/// list from the IR the source was emitted from keeps the runtime's copy of the size from ever
/// being transcribed by hand and drifting from the shader.

#include <vector>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/shader/IrModule.h"

namespace donner::gpu::shader {

/**
 * Returns every compute entry point \p module declares, with the workgroup size each was built
 * with, in declaration order. Empty for a module with no compute entry points.
 *
 * @param module Built IR module the shader source was emitted from.
 */
std::vector<ComputeEntryPointInfo> ComputeEntryPointsOf(const IrModule& module);

/**
 * Computes buffer requirements for each entry point from its reachable resource references and
 * the IR memory layout. Runtime arrays require at least one element and report their stride.
 * Unused bindings are omitted. The result is generated alongside source, never at draw time.
 *
 * @param module Built IR module the shader source was emitted from.
 */
ShaderResult<std::vector<ShaderBufferBindingInfo>> BufferBindingsOf(const IrModule& module);

}  // namespace donner::gpu::shader
