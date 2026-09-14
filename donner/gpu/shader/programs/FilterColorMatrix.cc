#include "donner/gpu/shader/programs/FilterColorMatrix.h"

#include "donner/gpu/shader/programs/FilterColorMatrixArtifactValidation.h"
#include "donner/gpu/shader/programs/FilterColorMatrixSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kFilterColorMatrixSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateFilterColorMatrixArtifact<kView>());
}  // namespace
const CompiledShaderView& FilterColorMatrixShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
