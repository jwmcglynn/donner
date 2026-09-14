#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"

#include "donner/gpu/shader/programs/SnapshotUnpremultiplyArtifactValidation.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiplySource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kSnapshotUnpremultiplySource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateSnapshotUnpremultiplyArtifact<kView>());
}  // namespace
const CompiledShaderView& SnapshotUnpremultiplyShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
