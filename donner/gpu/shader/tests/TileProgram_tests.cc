/// @file
/// Tile compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
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

std::string EmitTileSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildTileModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(TileProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildTileModule(), HasShaderResult());
}

TEST(TileProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitTileWgsl(), testing::Eq(EmitTileWgsl()));
  EXPECT_THAT(EmitTileMsl(), testing::Eq(EmitTileMsl()));
  EXPECT_THAT(EmitTileSpirvBytes(), testing::Eq(EmitTileSpirvBytes()));
}

TEST(TileProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitTileWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "tile.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("tile.wgsl")));
}

TEST(TileProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitTileMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "tile.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("tile.msl")));
}

TEST(TileProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitTileSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "tile.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("tile.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
