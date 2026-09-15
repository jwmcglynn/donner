#include "donner/gpu/shader/programs/SlugFill.h"

#include "donner/gpu/shader/programs/SlugFillArtifactValidation.h"
#include "donner/gpu/shader/programs/SlugFillSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kSlugFillSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateSlugFillArtifact<kView>());
}  // namespace

const CompiledShaderView& SlugFillShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
