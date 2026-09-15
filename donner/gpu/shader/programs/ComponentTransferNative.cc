#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/programs/ComponentTransferArtifactValidation.h"
#include "donner/gpu/shader/programs/ComponentTransferSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kComponentTransferSource, wgsl::kNativeProjection>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateComponentTransferArtifact<kView>());
}  // namespace
const CompiledShaderView& ComponentTransferNativeShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
