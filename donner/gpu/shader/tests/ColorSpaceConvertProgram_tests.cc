/// @file
/// Color space conversion program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and expose the expected program interfaces.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/ColorSpaceConvert.h"
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

std::vector<uint32_t> EmitColorSpaceConvertSpirv() {
  ShaderResult<IrModule> module = programs::BuildColorSpaceConvertModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(ColorSpaceConvertProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildColorSpaceConvertModule(), HasShaderResult());
}

TEST(ColorSpaceConvertProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitColorSpaceConvertWgsl(), testing::Eq(EmitColorSpaceConvertWgsl()));
  EXPECT_THAT(EmitColorSpaceConvertMsl(), testing::Eq(EmitColorSpaceConvertMsl()));
  EXPECT_THAT(EmitColorSpaceConvertSpirv(), testing::Eq(EmitColorSpaceConvertSpirv()));
}

TEST(ColorSpaceConvertProgramTests, WgslDeclaresTheComputeSurface) {
  const std::string wgsl = EmitColorSpaceConvertWgsl();

  EXPECT_THAT(wgsl, HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@builtin(global_invocation_id) gid: vec3<u32>"));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(0) var inputTexture: texture_2d<f32>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, "
                        "write>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(2) var<uniform> params: ColorSpaceConvertParams;"));
  // The three trailing words are load-bearing: without them the one u32 member sizes the block at
  // 4 bytes, and a host mirror declared with 16-byte alignment sizes the same member at 16.
  EXPECT_THAT(wgsl, HasSubstr("  pad0: u32,\n  pad1: u32,\n  pad2: u32,\n}"));
  // A compute entry point returns nothing, so no generated output struct may appear.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("cs_main_Output")));
}

TEST(ColorSpaceConvertProgramTests, TransferUsesBoundedSamplesInsteadOfDevicePow) {
  const std::string wgsl = EmitColorSpaceConvertWgsl();
  EXPECT_THAT(
      wgsl,
      HasSubstr("@group(0) @binding(3) var<storage, read> transferTable: ColorTransferTable"));
  EXPECT_THAT(wgsl, HasSubstr("if ((c > 0f))"));
  EXPECT_THAT(wgsl, HasSubstr("min(c, 1f)"));
  EXPECT_THAT(wgsl, HasSubstr("4095f"));
  EXPECT_THAT(wgsl, HasSubstr("4096u"));
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("pow(")));
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

}  // namespace
}  // namespace donner::gpu::shader
