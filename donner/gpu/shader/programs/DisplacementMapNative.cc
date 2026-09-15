#include "donner/gpu/shader/programs/DisplacementMap.h"
#include "donner/gpu/shader/programs/DisplacementMapArtifactValidation.h"
#include "donner/gpu/shader/programs/DisplacementMapSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kDisplacementMapSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateDisplacementMapArtifact<kView>());
}  // namespace
const CompiledShaderView& DisplacementMapNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
