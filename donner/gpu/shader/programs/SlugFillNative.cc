#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/gpu/shader/programs/SlugFillArtifactValidation.h"
#include "donner/gpu/shader/programs/SlugFillSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kSlugFillSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateSlugFillArtifact<kView>());
}  // namespace

const CompiledShaderView& SlugFillNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
