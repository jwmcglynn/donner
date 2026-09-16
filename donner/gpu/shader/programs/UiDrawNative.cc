#include "donner/gpu/shader/programs/UiDraw.h"
#include "donner/gpu/shader/programs/UiDrawArtifactValidation.h"
#include "donner/gpu/shader/programs/UiDrawSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kUiDrawSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateUiDrawArtifact<kView>());
}  // namespace
const CompiledShaderView& UiDrawNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
