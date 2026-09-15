#include "donner/gpu/shader/programs/SpecularLighting.h"

#include "donner/gpu/shader/programs/LightingArtifactValidation.h"
#include "donner/gpu/shader/programs/SpecularLightingSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kSpecularLightingSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateLightingArtifact<kView>());
}  // namespace

const CompiledShaderView& SpecularLightingShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
