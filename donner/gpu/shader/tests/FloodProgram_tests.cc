/// @file
/// Flood compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Flood.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitFloodWgsl() {
  ShaderResult<IrModule> module = programs::BuildFloodModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitFloodMsl() {
  ShaderResult<IrModule> module = programs::BuildFloodModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitFloodSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildFloodModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(FloodProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildFloodModule(), HasShaderResult());
}

TEST(FloodProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitFloodWgsl(), testing::Eq(EmitFloodWgsl()));
  EXPECT_THAT(EmitFloodMsl(), testing::Eq(EmitFloodMsl()));
  EXPECT_THAT(EmitFloodSpirvBytes(), testing::Eq(EmitFloodSpirvBytes()));
}

TEST(FloodProgramTests, WgslDeclaresTheComputeSurface) {
  const std::string wgsl = EmitFloodWgsl();

  EXPECT_THAT(wgsl, HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@builtin(global_invocation_id) gid: vec3<u32>"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(0) var outputTexture: texture_storage_2d<rgba8unorm, "
                        "write>;"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(1) var<uniform> params: FloodParams;"));
  EXPECT_THAT(wgsl, HasSubstr("textureStore(outputTexture, coords, params.color);"));
  // The destination is the only thing bounding the dispatch, so nothing may sample an input.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("texture_2d<f32>")));
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("textureLoad")));
  // A compute entry point returns nothing, so no generated output struct may appear.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("cs_main_Output")));
}

TEST(FloodProgramTests, MslDeclaresTheKernelSurface) {
  const std::string msl = EmitFloodMsl();

  EXPECT_THAT(msl, HasSubstr("kernel void cs_main("));
  EXPECT_THAT(msl, HasSubstr("uint3 gid [[thread_position_in_grid]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float, access::write> outputTexture [[texture(0)]]"));
  EXPECT_THAT(msl, HasSubstr("outputTexture.write(params.color, uint2(coords));"));
  // A kernel takes its builtins directly, so no stage-in struct may appear.
  EXPECT_THAT(msl, testing::Not(HasSubstr("stage_in")));
}

TEST(FloodProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitFloodWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "flood.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("flood.wgsl")));
}

TEST(FloodProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitFloodMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "flood.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("flood.msl")));
}

TEST(FloodProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitFloodSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "flood.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("flood.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
