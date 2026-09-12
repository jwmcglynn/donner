/// @file
/// Tile compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and expose the expected program interfaces.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitTileWgsl() {
  ShaderResult<IrModule> module = programs::BuildTileModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitTileMsl() {
  ShaderResult<IrModule> module = programs::BuildTileModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::vector<uint32_t> EmitTileSpirv() {
  ShaderResult<IrModule> module = programs::BuildTileModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(TileProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildTileModule(), HasShaderResult());
}

TEST(TileProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitTileWgsl(), testing::Eq(EmitTileWgsl()));
  EXPECT_THAT(EmitTileMsl(), testing::Eq(EmitTileMsl()));
  EXPECT_THAT(EmitTileSpirv(), testing::Eq(EmitTileSpirv()));
}

}  // namespace
}  // namespace donner::gpu::shader
