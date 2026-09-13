#include "donner/gpu/shader/wgsl/tests/GraphicsArtifact.h"

#include "donner/gpu/shader/wgsl/tests/GraphicsSource.h"
#include "donner/gpu/shader/wgsl/tests/MatrixSource.h"

namespace donner::gpu::shader::wgsl::tests {
namespace {
constexpr auto kArtifact = Compile<kGraphicsSource, Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(kView.entryPoints.size() == 2);
static_assert(kView.entryPoints[0].stage == ShaderStage::Vertex);
static_assert(kView.entryPoints[1].stage == ShaderStage::Fragment);
static_assert(kView.interfaceVariables.size() == 6);
constexpr auto kMatrixArtifact = Compile<kMatrixSource, Projection::All>();
constexpr CompiledShaderView kMatrixView = kMatrixArtifact.view();
static_assert(kMatrixView.matchesMember("params", "mvp", 0, 64, ShaderScalarType::F32, 4, 0, 0, 4,
                                        16));
}  // namespace

const CompiledShaderView& GraphicsShader() {
  return kView;
}

const CompiledShaderView& MatrixShader() {
  return kMatrixView;
}

}  // namespace donner::gpu::shader::wgsl::tests
