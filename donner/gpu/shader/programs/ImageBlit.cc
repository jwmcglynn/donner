#include "donner/gpu/shader/programs/ImageBlit.h"

#include "donner/gpu/shader/programs/ImageBlitArtifactValidation.h"
#include "donner/gpu/shader/programs/ImageBlitSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kImageBlitSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateImageBlitArtifact<kView>());
}  // namespace

const CompiledShaderView& ImageBlitShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
