#include "donner/gpu/shader/programs/Tile.h"

#include "donner/gpu/shader/programs/TileArtifactValidation.h"
#include "donner/gpu/shader/programs/TileSource.h"
namespace donner::gpu::shader::programs {
namespace {
constexpr auto kArtifact = wgsl::Compile<kTileSource, wgsl::Projection::Wgsl>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(ValidateTileArtifact<kView>());
}  // namespace
const CompiledShaderView& TileShader() {
  return kView;
}
}  // namespace donner::gpu::shader::programs
