#include "donner/gpu/shader/programs/FilterImage.h"
#include "donner/gpu/shader/programs/FilterImageArtifactValidation.h"
#include "donner/gpu/shader/programs/FilterImageSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kFilterImageSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateFilterImageArtifact<kView>());
}  // namespace
const CompiledShaderView& FilterImageNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
