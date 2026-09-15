#include "donner/gpu/shader/programs/Turbulence.h"

#include "donner/gpu/shader/programs/TurbulenceArtifactValidation.h"
#include "donner/gpu/shader/programs/TurbulenceSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kTurbulenceSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateTurbulenceArtifact<kView>());
}  // namespace

const CompiledShaderView& TurbulenceShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
