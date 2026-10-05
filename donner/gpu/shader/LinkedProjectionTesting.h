#pragma once
/// @file
/// Registration of alternate projections, for tests whose device consumes a projection the build
/// does not link. Declared in a test-only library, so no production target can register one.

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/LinkedProjection.h"

namespace donner::gpu::shader {

/**
 * Registers \p alternate as the view of \p linked's family for devices that consume \p kind.
 *
 * Registration must happen before any device that consumes \p kind selects \p linked; the
 * test-only alternates library does it from a static initializer. Registering the same alternate
 * again is a no-op, and registering a different one for the same family and kind is a fatal error.
 * Both views must outlive the process; artifact views do.
 *
 * @param linked The family's linked artifact, identified by address.
 * @param kind Source kind \p alternate carries.
 * @param alternate The family's artifact for \p kind.
 */
void RegisterAlternateProjection(const CompiledShaderView& linked, ShaderSourceKind kind,
                                 const CompiledShaderView& alternate);

}  // namespace donner::gpu::shader
