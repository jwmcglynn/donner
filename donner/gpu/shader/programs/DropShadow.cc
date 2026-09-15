#include "donner/gpu/shader/programs/DropShadow.h"

#include "donner/gpu/shader/programs/DropShadowArtifactValidation.h"
#include "donner/gpu/shader/programs/DropShadowSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kDropShadowSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateDropShadowArtifact<kView>());
}  // namespace
const CompiledShaderView& DropShadowShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
