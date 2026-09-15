#pragma once
/// @file
/// Selects the shader projection a device consumes from a family's linked artifacts.

#include <string_view>

#include "donner/base/Utils.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/shader/CompiledShader.h"

/**
 * The platform-native artifact of \p family, or null on a build that links none.
 *
 * Only the platforms with a native device link the native artifacts, so the native accessors
 * have no definition in the WebAssembly package and must not be named there. The caller includes
 * the family's own program header; this only names the accessor.
 *
 * @param family Program name, as in `SlugFill` for `SlugFillShader` and `SlugFillNativeShader`.
 */
#if (defined(__APPLE__) || defined(__linux__)) && !defined(__EMSCRIPTEN__)
#define DONNER_GEODE_NATIVE_SHADER(family) \
  (&::donner::gpu::shader::programs::family##NativeShader())
#else
#define DONNER_GEODE_NATIVE_SHADER(family) (nullptr)
#endif

namespace donner::geode {

/**
 * Selects one family's projection for \p device: the platform-native artifact for a Metal or
 * Vulkan device, and the authored WGSL artifact for every other device.
 *
 * A build that links no native artifact passes a null \p nativeShader, so a native device there
 * selects a view whose projection is empty. \ref gpu::Device::createShaderModule refuses that
 * descriptor rather than compiling nothing, which keeps the mismatch fail-closed.
 *
 * Callers that also derive bindings, entry points or workgroup shapes from reflection must read
 * them from the returned view, so source and interface always come from the same artifact.
 *
 * @param device Device whose source kind selects the projection.
 * @param wgslShader Authored WGSL artifact.
 * @param nativeShader Platform-native artifact, or null when this build links none.
 * @return The artifact view to build a descriptor from.
 */
inline const gpu::shader::CompiledShaderView& SelectShaderProjection(
    const gpu::Device& device,
    const gpu::shader::CompiledShaderView& wgslShader UTILS_LIFETIME_BOUND,
    const gpu::shader::CompiledShaderView* nativeShader UTILS_LIFETIME_BOUND) {
  if (device.shaderSourceKind() == gpu::ShaderSourceKind::Wgsl || nativeShader == nullptr) {
    return wgslShader;
  }
  return *nativeShader;
}

/**
 * Creates one family's shader module from the projection \p device consumes.
 *
 * @param device GPU device receiving the prevalidated source and metadata.
 * @param wgslShader Authored WGSL artifact.
 * @param nativeShader Platform-native artifact, or null when this build links none.
 * @param label Diagnostic shader label.
 * @return Shader module or creation error.
 */
inline gpu::Result<gpu::ShaderModule> CreateShaderModule(
    gpu::Device& device, const gpu::shader::CompiledShaderView& wgslShader,
    const gpu::shader::CompiledShaderView* nativeShader, std::string_view label) {
  return device.createShaderModule(gpu::shader::MakeShaderDescriptor(
      SelectShaderProjection(device, wgslShader, nativeShader), device.shaderSourceKind(), label));
}

}  // namespace donner::geode
