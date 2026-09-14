#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/programs/FloodArtifactValidation.h"
#include "donner/gpu/shader/programs/FloodSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kFloodSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateFloodArtifact<kView>());
}  // namespace
const CompiledShaderView& FloodNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
