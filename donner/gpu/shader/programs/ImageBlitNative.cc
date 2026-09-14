#include "donner/gpu/shader/programs/ImageBlit.h"
#include "donner/gpu/shader/programs/ImageBlitArtifactValidation.h"
#include "donner/gpu/shader/programs/ImageBlitSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kImageBlitSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateImageBlitArtifact<kView>());
}  // namespace

const CompiledShaderView& ImageBlitNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
