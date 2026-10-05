#pragma once
/// @file
/// The one projection of each shader family a build links, and the view a device builds its shader
/// module from.

#include "donner/base/Utils.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/shader/CompiledShader.h"

/**
 * Names the artifact of \p family this build links for its own devices: the platform-native
 * projection (MSL on Apple platforms, SPIR-V on Linux) in a native build, and the authored WGSL in
 * the WebAssembly package. Production code names a family's artifact only through this macro and
 * depends on the family through `linked_shader_artifacts`, so no build links a projection none of
 * its devices consume. The caller includes the family's own program header.
 *
 * @param family Program name, as in `SlugFill` for `SlugFillNativeShader` and `SlugFillShader`.
 */
#if !defined(__EMSCRIPTEN__) && defined(__APPLE__)
#define DONNER_LINKED_SHADER_ARTIFACT(family) \
  (::donner::gpu::shader::programs::family##NativeShader())
#define DONNER_LINKED_SHADER_SOURCE_KIND Msl
#elif !defined(__EMSCRIPTEN__) && defined(__linux__)
#define DONNER_LINKED_SHADER_ARTIFACT(family) \
  (::donner::gpu::shader::programs::family##NativeShader())
#define DONNER_LINKED_SHADER_SOURCE_KIND Spirv
#else
#define DONNER_LINKED_SHADER_ARTIFACT(family) (::donner::gpu::shader::programs::family##Shader())
#define DONNER_LINKED_SHADER_SOURCE_KIND Wgsl
#endif

namespace donner::gpu::shader {

/// Source kind of the artifacts \ref DONNER_LINKED_SHADER_ARTIFACT names in this build.
inline constexpr ShaderSourceKind kLinkedShaderSourceKind =
    ShaderSourceKind::DONNER_LINKED_SHADER_SOURCE_KIND;
#undef DONNER_LINKED_SHADER_SOURCE_KIND

/**
 * Selects the view a device that consumes \p deviceKind builds one family's module from, given
 * the artifact of that family this build links.
 *
 * A Metal or Vulkan device in a native build, and the browser device in the WebAssembly package,
 * consume the linked projection and receive \p linked. A device that consumes another projection
 * exists only in tests, such as a recording device or the WebGPU reference on a native build: it
 * receives the view a test-only library registered for \p linked through
 * `RegisterAlternateProjection` (`LinkedProjectionTesting.h`, a test-only library, so no
 * production target can register one). Without one it also receives \p linked, which carries
 * nothing in its kind, so `gpu::Device::createShaderModule` refuses the empty descriptor rather
 * than compiling nothing.
 *
 * Callers that derive bindings, entry points or workgroup shapes from reflection may read them from
 * \p linked: every projection of a family is compiled with one reflected interface.
 *
 * @param deviceKind Source kind the device consumes.
 * @param linked The family's artifact this build links.
 * @return The artifact view to build a shader module descriptor from.
 */
const CompiledShaderView& SelectLinkedProjection(
    ShaderSourceKind deviceKind, const CompiledShaderView& linked UTILS_LIFETIME_BOUND);

}  // namespace donner::gpu::shader
