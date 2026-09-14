#include "donner/gpu/shader/programs/Checkerboard.h"
#include "donner/gpu/shader/programs/CheckerboardArtifactValidation.h"
#include "donner/gpu/shader/programs/CheckerboardSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kCheckerboardSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateCheckerboardArtifact<kView>());
}  // namespace
const CompiledShaderView& CheckerboardNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
