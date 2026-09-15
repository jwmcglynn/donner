#include "donner/gpu/shader/programs/FilterBlend.h"
#include "donner/gpu/shader/programs/FilterBlendArtifactValidation.h"
#include "donner/gpu/shader/programs/FilterBlendSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kFilterBlendSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateFilterBlendArtifact<kView>());
}  // namespace
const CompiledShaderView& FilterBlendNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
