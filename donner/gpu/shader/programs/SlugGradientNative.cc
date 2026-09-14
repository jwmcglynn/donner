#include "donner/gpu/shader/programs/SlugGradient.h"
#include "donner/gpu/shader/programs/SlugGradientArtifactValidation.h"
#include "donner/gpu/shader/programs/SlugGradientSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kSlugGradientSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateSlugGradientArtifact<kView>());
}  // namespace
const CompiledShaderView& SlugGradientNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
