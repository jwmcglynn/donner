#include "donner/gpu/shader/wgsl/tests/GraphicsArtifact.h"

#include "donner/gpu/shader/wgsl/tests/GraphicsSource.h"

namespace donner::gpu::shader::wgsl::tests {
namespace {
constexpr auto kArtifact = Compile<kGraphicsSource, Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(kView.entryPoints.size() == 2);
static_assert(kView.entryPoints[0].stage == ShaderStage::Vertex);
static_assert(kView.entryPoints[1].stage == ShaderStage::Fragment);
static_assert(kView.interfaceVariables.size() == 6);
}  // namespace

const CompiledShaderView& GraphicsShader() {
  return kView;
}

}  // namespace donner::gpu::shader::wgsl::tests
