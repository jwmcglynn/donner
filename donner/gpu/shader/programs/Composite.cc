#include "donner/gpu/shader/programs/Composite.h"

#include "donner/gpu/shader/programs/CompositeArtifactValidation.h"
#include "donner/gpu/shader/programs/CompositeSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kCompositeSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateCompositeArtifact<kView>());
}  // namespace
const CompiledShaderView& CompositeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
