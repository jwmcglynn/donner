#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/programs/GaussianBlurArtifactValidation.h"
#include "donner/gpu/shader/programs/GaussianBlurProjection.h"
#include "donner/gpu/shader/programs/GaussianBlurSource.h"

namespace donner::gpu::shader::programs {
namespace {

#if !defined(__EMSCRIPTEN__) && (defined(__APPLE__) || defined(__linux__) || defined(_WIN32))
constexpr auto kGaussianBlurNativeArtifact =
    wgsl::Compile<kGaussianBlurSource, kGaussianBlurNativeProjection>();
constexpr CompiledShaderView kGaussianBlurNativeView = kGaussianBlurNativeArtifact.view();
static_assert(ValidateGaussianBlurArtifact<kGaussianBlurNativeView>());
#else
#error "GaussianBlurNativeShader is only built for a native backend"
#endif

}  // namespace

const CompiledShaderView& GaussianBlurNativeShader() {
  return kGaussianBlurNativeView;
}

}  // namespace donner::gpu::shader::programs
