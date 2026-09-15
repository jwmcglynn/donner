#include "donner/svg/renderer/geode/GeodeShaders.h"

#include <string_view>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugMask.h"

// Only the platforms with a native device link the native artifacts, so the native accessors
// have no definition in the WebAssembly package and must not be named there.
#if (defined(__APPLE__) || defined(__linux__)) && !defined(__EMSCRIPTEN__)
#define DONNER_GEODE_NATIVE_SHADER(family) (&gpu::shader::programs::family##NativeShader())
#else
#define DONNER_GEODE_NATIVE_SHADER(family) (nullptr)
#endif

namespace donner::geode {

namespace {

/// Creates one family's module from the projection \p device consumes: the platform-native
/// artifact for a Metal or Vulkan device, and the authored WGSL artifact for every other device.
/// A native device on a build that links no native artifact gets an empty projection, which
/// \ref gpu::Device::createShaderModule refuses rather than compiling nothing.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @param wgslShader Authored WGSL artifact.
/// @param nativeShader Platform-native artifact, or null when this build links none.
/// @param label Diagnostic shader label.
gpu::Result<gpu::ShaderModule> CreateShaderModule(
    gpu::Device& device, const gpu::shader::CompiledShaderView& wgslShader,
    const gpu::shader::CompiledShaderView* nativeShader, std::string_view label) {
  const gpu::ShaderSourceKind kind = device.shaderSourceKind();
  const gpu::shader::CompiledShaderView& shader =
      (kind == gpu::ShaderSourceKind::Wgsl || nativeShader == nullptr) ? wgslShader : *nativeShader;
  return device.createShaderModule(gpu::shader::MakeShaderDescriptor(shader, kind, label));
}

}  // namespace

gpu::Result<gpu::ShaderModule> createSlugFillShader(gpu::Device& device) {
  return CreateShaderModule(device, gpu::shader::programs::SlugFillShader(),
                            DONNER_GEODE_NATIVE_SHADER(SlugFill), "Slug fill");
}

gpu::Result<gpu::ShaderModule> createSlugGradientShader(gpu::Device& device) {
  return CreateShaderModule(device, gpu::shader::programs::SlugGradientShader(),
                            DONNER_GEODE_NATIVE_SHADER(SlugGradient), "SlugGradient");
}

gpu::Result<gpu::ShaderModule> createSlugMaskShader(gpu::Device& device) {
  return CreateShaderModule(device, gpu::shader::programs::SlugMaskShader(),
                            DONNER_GEODE_NATIVE_SHADER(SlugMask), "SlugMask");
}

gpu::Result<gpu::ShaderModule> createImageBlitShader(gpu::Device& device) {
  return CreateShaderModule(device, gpu::shader::programs::ImageBlitShader(),
                            DONNER_GEODE_NATIVE_SHADER(ImageBlit), "ImageBlit");
}

}  // namespace donner::geode

#undef DONNER_GEODE_NATIVE_SHADER
