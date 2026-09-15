#include "donner/gpu/shader/programs/FilterResolve.h"
#include "donner/gpu/shader/programs/FilterResolveArtifactValidation.h"
#include "donner/gpu/shader/programs/FilterResolveSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kFilterResolveSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateFilterResolveArtifact<kView>());
}  // namespace

const CompiledShaderView& FilterResolveNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
