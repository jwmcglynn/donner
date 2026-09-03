/// @file
/// Color space conversion program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitColorSpaceConvertWgsl() {
  ShaderResult<IrModule> module = programs::BuildColorSpaceConvertModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitColorSpaceConvertMsl() {
  ShaderResult<IrModule> module = programs::BuildColorSpaceConvertModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitColorSpaceConvertSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildColorSpaceConvertModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(ColorSpaceConvertProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildColorSpaceConvertModule(), HasShaderResult());
}

TEST(ColorSpaceConvertProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitColorSpaceConvertWgsl(), testing::Eq(EmitColorSpaceConvertWgsl()));
  EXPECT_THAT(EmitColorSpaceConvertMsl(), testing::Eq(EmitColorSpaceConvertMsl()));
  EXPECT_THAT(EmitColorSpaceConvertSpirvBytes(), testing::Eq(EmitColorSpaceConvertSpirvBytes()));
}

TEST(ColorSpaceConvertProgramTests, WgslDeclaresTheComputeSurface) {
  const std::string wgsl = EmitColorSpaceConvertWgsl();

  EXPECT_THAT(wgsl, HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@builtin(global_invocation_id) gid: vec3<u32>"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(0) var inputTexture: texture_2d<f32>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba8unorm, "
                        "write>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(2) var<uniform> params: ColorSpaceConvertParams;"));
  // The three trailing words are load-bearing: without them the one u32 member sizes the block at
  // 4 bytes, and a host mirror declared with 16-byte alignment sizes the same member at 16.
  EXPECT_THAT(wgsl, HasSubstr("  pad0: u32,\n  pad1: u32,\n  pad2: u32,\n}"));
  // A compute entry point returns nothing, so no generated output struct may appear.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("cs_main_Output")));
}

TEST(ColorSpaceConvertProgramTests, WgslSpellsTheSrgbTransferExactly) {
  const std::string wgsl = EmitColorSpaceConvertWgsl();

  // Every constant of the transfer is pinned here, in both directions. These are the numbers the
  // CPU filter path uses; a transfer that drifted from them by one digit would still look
  // plausible and would disagree with the reference on every texel.
  EXPECT_THAT(wgsl, HasSubstr("fn srgb_channel_to_linear(c: f32) -> f32 {\n"
                              "  if ((c <= 0.04045f)) {\n"
                              "    return (c / 12.92f);\n"
                              "  }\n"
                              "  return pow(((c + 0.055f) / 1.055f), 2.4f);\n"
                              "}"));
  EXPECT_THAT(wgsl, HasSubstr("fn linear_channel_to_srgb(c: f32) -> f32 {\n"
                              "  if ((c <= 0.0031308f)) {\n"
                              "    return (c * 12.92f);\n"
                              "  }\n"
                              "  return ((1.055f * pow(c, 0.41666666f)) - 0.055f);\n"
                              "}"));
}

TEST(ColorSpaceConvertProgramTests, WgslBranchesRatherThanSelectsOverThePowCurve) {
  const std::string wgsl = EmitColorSpaceConvertWgsl();

  // pow is undefined for a negative base, and select evaluates both of its arms. The linear
  // segment is what covers the inputs the curve cannot take, so it has to be a branch.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("select(")));
}

TEST(ColorSpaceConvertProgramTests, MslDeclaresTheKernelSurface) {
  const std::string msl = EmitColorSpaceConvertMsl();

  EXPECT_THAT(msl, HasSubstr("kernel void cs_main("));
  EXPECT_THAT(msl, HasSubstr("uint3 gid [[thread_position_in_grid]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float> inputTexture [[texture(0)]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float, access::write> outputTexture [[texture(1)]]"));
  // A kernel takes its builtins directly, so no stage-in struct may appear.
  EXPECT_THAT(msl, testing::Not(HasSubstr("stage_in")));
}

TEST(ColorSpaceConvertProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitColorSpaceConvertWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "color_space_convert.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("color_space_convert.wgsl")));
}

TEST(ColorSpaceConvertProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitColorSpaceConvertMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "color_space_convert.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("color_space_convert.msl")));
}

TEST(ColorSpaceConvertProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitColorSpaceConvertSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "color_space_convert.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("color_space_convert.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
