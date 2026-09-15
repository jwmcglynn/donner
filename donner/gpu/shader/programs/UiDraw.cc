#include "donner/gpu/shader/programs/UiDraw.h"

#include "donner/gpu/shader/programs/UiDrawArtifactValidation.h"
#include "donner/gpu/shader/programs/UiDrawSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kUiDrawSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateUiDrawArtifact<kView>());
}  // namespace
const CompiledShaderView& UiDrawShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
