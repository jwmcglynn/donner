#include "donner/gpu/shader/programs/SpecularLighting.h"
#include "donner/gpu/shader/programs/SpecularLightingArtifactValidation.h"
#include "donner/gpu/shader/programs/SpecularLightingSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kSpecularLightingSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateSpecularLightingArtifact<kView>());
}  // namespace

const CompiledShaderView& SpecularLightingNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
