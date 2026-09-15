#include "donner/gpu/shader/programs/SubregionClip.h"

#include "donner/gpu/shader/programs/SubregionClipArtifactValidation.h"
#include "donner/gpu/shader/programs/SubregionClipSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kSubregionClipSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateSubregionClipArtifact<kView>());
}  // namespace
const CompiledShaderView& SubregionClipShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
