/// @file
/// Offset compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitOffsetWgsl() {
  ShaderResult<IrModule> module = programs::BuildOffsetModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitOffsetMsl() {
  ShaderResult<IrModule> module = programs::BuildOffsetModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitOffsetSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildOffsetModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(OffsetProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildOffsetModule(), HasShaderResult());
}

TEST(OffsetProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitOffsetWgsl(), testing::Eq(EmitOffsetWgsl()));
  EXPECT_THAT(EmitOffsetMsl(), testing::Eq(EmitOffsetMsl()));
  EXPECT_THAT(EmitOffsetSpirvBytes(), testing::Eq(EmitOffsetSpirvBytes()));
}

TEST(OffsetProgramTests, WgslDeclaresTheComputeSurface) {
  const std::string wgsl = EmitOffsetWgsl();

  EXPECT_THAT(wgsl, HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@builtin(global_invocation_id) gid: vec3<u32>"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(0) var inputTexture: texture_2d<f32>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba8unorm, "
                        "write>;"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(2) var<uniform> params: OffsetParams;"));
  // The two trailing words are load-bearing: without them the two f32 members size the block at
  // 8 bytes, and a host mirror declared with 16-byte alignment sizes the same members at 16.
  EXPECT_THAT(wgsl, HasSubstr("  pad0: u32,\n  pad1: u32,\n}"));
  // A compute entry point returns nothing, so no generated output struct may appear.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("cs_main_Output")));
}

TEST(OffsetProgramTests, WgslGuardsTheDestinationExtent) {
  const std::string wgsl = EmitOffsetWgsl();

  // A dispatch is rounded up to whole workgroups, so lanes run past both edges of a destination
  // whose extent is not a multiple of the workgroup size. WGSL discards an out-of-bounds
  // textureStore, so no device comparison can see this guard go missing; it is named here
  // instead, where its removal is a failure rather than a golden diff nobody reads.
  EXPECT_THAT(wgsl, HasSubstr("  let extent = textureDimensions(outputTexture);\n"
                              "  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {\n"
                              "    return;\n"
                              "  }"));
}

TEST(OffsetProgramTests, WgslRoundsHalvesAwayFromZeroRatherThanToEven) {
  const std::string wgsl = EmitOffsetWgsl();

  // The recipe is declared once and called twice. WGSL's own round() is round-half-to-even, so
  // its appearance anywhere in this program would be the rounding rule silently changing.
  EXPECT_THAT(wgsl, HasSubstr("fn round_half_away_from_zero(x: f32) -> f32 {\n"
                              "  return (sign(x) * floor((abs(x) + 0.5f)));\n"
                              "}"));
  EXPECT_THAT(wgsl, HasSubstr("round_half_away_from_zero(params.dx)"));
  EXPECT_THAT(wgsl, HasSubstr("round_half_away_from_zero(params.dy)"));
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("round(")));
}

TEST(OffsetProgramTests, WgslWritesBothArmsOfTheEdgeTest) {
  const std::string wgsl = EmitOffsetWgsl();

  // Both arms write: a source texel outside the input is transparent black, one inside is the
  // untouched source texel. A missing else would leave the exposed edge undefined.
  EXPECT_THAT(wgsl, HasSubstr("textureStore(outputTexture, coords, vec4<f32>(0f, 0f, 0f, 0f));"));
  EXPECT_THAT(wgsl, HasSubstr("} else {"));
  EXPECT_THAT(
      wgsl,
      HasSubstr("textureStore(outputTexture, coords, textureLoad(inputTexture, source, 0i));"));
}

TEST(OffsetProgramTests, MslDeclaresTheKernelSurface) {
  const std::string msl = EmitOffsetMsl();

  EXPECT_THAT(msl, HasSubstr("kernel void cs_main("));
  EXPECT_THAT(msl, HasSubstr("uint3 gid [[thread_position_in_grid]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float> inputTexture [[texture(0)]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float, access::write> outputTexture [[texture(1)]]"));
  EXPECT_THAT(msl,
              HasSubstr("outputTexture.write(float4(0.0f, 0.0f, 0.0f, 0.0f), uint2(coords));"));
  // A kernel takes its builtins directly, so no stage-in struct may appear.
  EXPECT_THAT(msl, testing::Not(HasSubstr("stage_in")));
}

TEST(OffsetProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitOffsetWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "offset.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("offset.wgsl")));
}

TEST(OffsetProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitOffsetMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "offset.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("offset.msl")));
}

TEST(OffsetProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitOffsetSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "offset.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("offset.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
