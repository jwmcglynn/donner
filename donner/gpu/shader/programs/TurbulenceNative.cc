#include "donner/gpu/shader/programs/Turbulence.h"
#include "donner/gpu/shader/programs/TurbulenceArtifactValidation.h"
#include "donner/gpu/shader/programs/TurbulenceSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kTurbulenceSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateTurbulenceArtifact<kView>());
}  // namespace

const CompiledShaderView& TurbulenceNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
