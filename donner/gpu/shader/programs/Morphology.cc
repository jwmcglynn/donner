#include "donner/gpu/shader/programs/Morphology.h"

#include "donner/gpu/shader/programs/MorphologyArtifactValidation.h"
#include "donner/gpu/shader/programs/MorphologySource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kMorphologySource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateMorphologyArtifact<kView>());
}  // namespace
const CompiledShaderView& MorphologyShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
