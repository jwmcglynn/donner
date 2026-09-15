#include "donner/gpu/shader/programs/ConvolveMatrix.h"

#include "donner/gpu/shader/programs/ConvolveMatrixArtifactValidation.h"
#include "donner/gpu/shader/programs/ConvolveMatrixSource.h"

namespace donner::gpu::shader::programs {
namespace {

constexpr auto kArtifact = wgsl::Compile<kConvolveMatrixSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateConvolveMatrixArtifact<kView>());

}  // namespace

const CompiledShaderView& ConvolveMatrixShader() {
  return kView;
}

}  // namespace donner::gpu::shader::programs
