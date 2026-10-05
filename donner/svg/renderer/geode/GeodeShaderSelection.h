#pragma once
/// @file
/// Selects the shader projection a device consumes from a family's linked artifact.

#include <string_view>

#include "donner/base/Utils.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/LinkedProjection.h"

namespace donner::geode {

/**
 * Selects one family's projection for \p device from the artifact this build links for it, named
 * with `DONNER_LINKED_SHADER_ARTIFACT`.
 *
 * A native build links only the platform-native projection, which its Metal or Vulkan device
 * consumes; the WebAssembly package links only the authored WGSL its browser device consumes. A
 * test device that consumes another projection receives the one a test-only library registered,
 * and otherwise an artifact that carries nothing in its kind, which
 * \ref donner::gpu::Device::createShaderModule "gpu::Device::createShaderModule" refuses rather
 * than compiling nothing. See \ref donner::gpu::shader::SelectLinkedProjection
 * "gpu::shader::SelectLinkedProjection".
 *
 * Every projection of a family shares one reflected interface, so bindings, entry points and
 * workgroup shapes may be read from either the returned view or \p linked.
 *
 * @param device Device whose source kind selects the projection.
 * @param linked The family's linked artifact.
 * @return The artifact view to build a descriptor from.
 */
inline const gpu::shader::CompiledShaderView& SelectShaderProjection(
    const gpu::Device& device, const gpu::shader::CompiledShaderView& linked UTILS_LIFETIME_BOUND) {
  return gpu::shader::SelectLinkedProjection(device.shaderSourceKind(), linked);
}

/**
 * Creates one family's shader module from the projection \p device consumes.
 *
 * @param device GPU device receiving the prevalidated source and metadata.
 * @param linked The family's linked artifact, named with `DONNER_LINKED_SHADER_ARTIFACT`.
 * @param label Diagnostic shader label.
 * @return Shader module or creation error.
 */
inline gpu::Result<gpu::ShaderModule> CreateShaderModule(
    gpu::Device& device, const gpu::shader::CompiledShaderView& linked, std::string_view label) {
  return device.createShaderModule(gpu::shader::MakeShaderDescriptor(
      SelectShaderProjection(device, linked), device.shaderSourceKind(), label));
}

}  // namespace donner::geode
