/// @file
/// Subregion-clip compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SubregionClip.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitSubregionClipWgsl() {
  ShaderResult<IrModule> module = programs::BuildSubregionClipModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitSubregionClipMsl() {
  ShaderResult<IrModule> module = programs::BuildSubregionClipModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitSubregionClipSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildSubregionClipModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(SubregionClipProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildSubregionClipModule(), HasShaderResult());
}

TEST(SubregionClipProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitSubregionClipWgsl(), testing::Eq(EmitSubregionClipWgsl()));
  EXPECT_THAT(EmitSubregionClipMsl(), testing::Eq(EmitSubregionClipMsl()));
  EXPECT_THAT(EmitSubregionClipSpirvBytes(), testing::Eq(EmitSubregionClipSpirvBytes()));
}

TEST(SubregionClipProgramTests, WgslDeclaresTheComputeSurface) {
  const std::string wgsl = EmitSubregionClipWgsl();

  EXPECT_THAT(wgsl, HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@builtin(global_invocation_id) gid: vec3<u32>"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(0) var inputTexture: texture_2d<f32>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba8unorm, "
                        "write>;"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(2) var<uniform> params: SubregionClipParams;"));
  // Both arms of the clip write: outside is transparent black, inside is the untouched source
  // texel. A missing else would leave the destination undefined outside the region.
  EXPECT_THAT(wgsl, HasSubstr("textureStore(outputTexture, coords, vec4<f32>(0f, 0f, 0f, 0f));"));
  EXPECT_THAT(wgsl, HasSubstr("} else {"));
  EXPECT_THAT(
      wgsl,
      HasSubstr("textureStore(outputTexture, coords, textureLoad(inputTexture, coords, 0i));"));
  // A compute entry point returns nothing, so no generated output struct may appear.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("cs_main_Output")));
}

TEST(SubregionClipProgramTests, MslDeclaresTheKernelSurface) {
  const std::string msl = EmitSubregionClipMsl();

  EXPECT_THAT(msl, HasSubstr("kernel void cs_main("));
  EXPECT_THAT(msl, HasSubstr("uint3 gid [[thread_position_in_grid]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float> inputTexture [[texture(0)]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float, access::write> outputTexture [[texture(1)]]"));
  EXPECT_THAT(msl,
              HasSubstr("outputTexture.write(float4(0.0f, 0.0f, 0.0f, 0.0f), uint2(coords));"));
  // A kernel takes its builtins directly, so no stage-in struct may appear.
  EXPECT_THAT(msl, testing::Not(HasSubstr("stage_in")));
}

TEST(SubregionClipProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitSubregionClipWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "subregion_clip.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("subregion_clip.wgsl")));
}

TEST(SubregionClipProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitSubregionClipMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "subregion_clip.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("subregion_clip.msl")));
}

TEST(SubregionClipProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitSubregionClipSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "subregion_clip.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("subregion_clip.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
