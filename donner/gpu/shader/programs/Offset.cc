#include "donner/gpu/shader/programs/Offset.h"

#include "donner/gpu/shader/programs/OffsetArtifactValidation.h"
#include "donner/gpu/shader/programs/OffsetSource.h"

namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kOffsetSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateOffsetArtifact<kView>());
}  // namespace

const CompiledShaderView& OffsetShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
