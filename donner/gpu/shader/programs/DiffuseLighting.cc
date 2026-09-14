#include "donner/gpu/shader/programs/DiffuseLighting.h"

#include "donner/gpu/shader/programs/DiffuseLightingArtifactValidation.h"
#include "donner/gpu/shader/programs/DiffuseLightingSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kDiffuseLightingSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateDiffuseLightingArtifact<kView>());
}  // namespace
const CompiledShaderView& DiffuseLightingShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
