#include "donner/gpu/shader/programs/ColorSpaceConvert.h"

#include "donner/gpu/shader/programs/ColorSpaceConvertArtifactValidation.h"
#include "donner/gpu/shader/programs/ColorSpaceConvertSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kColorSpaceConvertSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateColorSpaceConvertArtifact<kView>());
}  // namespace
const CompiledShaderView& ColorSpaceConvertShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
