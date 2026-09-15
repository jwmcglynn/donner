#include "donner/gpu/shader/programs/FilterImage.h"

#include "donner/gpu/shader/programs/FilterImageArtifactValidation.h"
#include "donner/gpu/shader/programs/FilterImageSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kFilterImageSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateFilterImageArtifact<kView>());
}  // namespace
const CompiledShaderView& FilterImageShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
