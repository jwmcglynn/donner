#include "donner/gpu/shader/programs/Merge.h"

#include "donner/gpu/shader/programs/MergeArtifactValidation.h"
#include "donner/gpu/shader/programs/MergeSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kMergeSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateMergeArtifact<kView>());
}  // namespace
const CompiledShaderView& MergeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
