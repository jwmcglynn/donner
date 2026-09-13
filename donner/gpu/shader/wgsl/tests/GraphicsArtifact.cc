#include "donner/gpu/shader/wgsl/tests/GraphicsArtifact.h"

#include "donner/gpu/shader/wgsl/tests/ControlSource.h"
#include "donner/gpu/shader/wgsl/tests/GraphicsSource.h"
#include "donner/gpu/shader/wgsl/tests/MatrixSource.h"
#include "donner/gpu/shader/wgsl/tests/StorageArraySource.h"

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
constexpr auto kOperationsArtifact = Compile<kMatrixOperationsSource, Projection::All>();
constexpr CompiledShaderView kOperationsView = kOperationsArtifact.view();
constexpr auto kStorageArtifact = Compile<kStorageArraySource, Projection::All>();
constexpr CompiledShaderView kStorageView = kStorageArtifact.view();
static_assert(kStorageView.resource("bands")->runtimeArrayStrideBytes == 8);
static_assert(kStorageView.matchesMember("params", "vertices", 0, 64, ShaderScalarType::F32, 4, 4,
                                         16));
constexpr auto kControlArtifact = Compile<kControlSource, Projection::All>();
constexpr CompiledShaderView kControlView = kControlArtifact.view();
}  // namespace

const CompiledShaderView& GraphicsShader() {
  return kView;
}

const CompiledShaderView& MatrixShader() {
  return kMatrixView;
}

const CompiledShaderView& MatrixOperationsShader() {
  return kOperationsView;
}

const CompiledShaderView& StorageArrayShader() {
  return kStorageView;
}

const CompiledShaderView& ControlShader() {
  return kControlView;
}

}  // namespace donner::gpu::shader::wgsl::tests
