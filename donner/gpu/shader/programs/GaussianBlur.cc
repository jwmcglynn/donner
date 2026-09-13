#include "donner/gpu/shader/programs/GaussianBlur.h"

#include "donner/gpu/shader/programs/GaussianBlurArtifactValidation.h"
#include "donner/gpu/shader/programs/GaussianBlurProjection.h"
#include "donner/gpu/shader/programs/GaussianBlurSource.h"

namespace donner::gpu::shader::programs {
namespace {

constexpr auto kGaussianBlurArtifact =
    wgsl::Compile<kGaussianBlurSource, kGaussianBlurGeodeProjection>();

constexpr CompiledShaderView kGaussianBlurView = kGaussianBlurArtifact.view();
static_assert(ValidateGaussianBlurArtifact<kGaussianBlurView>());

}  // namespace

const CompiledShaderView& GaussianBlurShader() {
  return kGaussianBlurView;
}

}  // namespace donner::gpu::shader::programs
