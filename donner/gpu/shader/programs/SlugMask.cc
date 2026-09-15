#include "donner/gpu/shader/programs/SlugMask.h"

#include "donner/gpu/shader/programs/SlugMaskArtifactValidation.h"
#include "donner/gpu/shader/programs/SlugMaskSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kSlugMaskSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateSlugMaskArtifact<kView>());
}  // namespace

const CompiledShaderView& SlugMaskShader() {
  return kView;
}

}  // namespace donner::gpu::shader::programs
