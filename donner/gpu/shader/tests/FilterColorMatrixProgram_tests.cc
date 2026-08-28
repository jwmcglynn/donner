/// @file
/// Color-matrix filter program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/FilterColorMatrix.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitFilterColorMatrixWgsl() {
  ShaderResult<IrModule> module = programs::BuildFilterColorMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitFilterColorMatrixMsl() {
  ShaderResult<IrModule> module = programs::BuildFilterColorMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitFilterColorMatrixSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildFilterColorMatrixModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(FilterColorMatrixProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildFilterColorMatrixModule(), HasShaderResult());
}

TEST(FilterColorMatrixProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitFilterColorMatrixWgsl(), testing::Eq(EmitFilterColorMatrixWgsl()));
  EXPECT_THAT(EmitFilterColorMatrixMsl(), testing::Eq(EmitFilterColorMatrixMsl()));
  EXPECT_THAT(EmitFilterColorMatrixSpirvBytes(), testing::Eq(EmitFilterColorMatrixSpirvBytes()));
}

TEST(FilterColorMatrixProgramTests, WgslDeclaresTheComputeSurface) {
  const std::string wgsl = EmitFilterColorMatrixWgsl();

  EXPECT_THAT(wgsl, HasSubstr("@compute @workgroup_size(8, 8, 1)\nfn cs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@group(0) @binding(0) var inputTexture: texture_2d<f32>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba8unorm, "
                        "write>;"));
  EXPECT_THAT(wgsl,
              HasSubstr("@group(0) @binding(2) var<uniform> params: FilterColorMatrixParams;"));
  // All five columns must reach the result: four multipliers and the constant one.
  for (const char* column : {"col0", "col1", "col2", "col3", "col4"}) {
    EXPECT_THAT(wgsl, HasSubstr(std::string("params.") + column));
  }
  // A compute entry point returns nothing, so no generated output struct may appear.
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("cs_main_Output")));
}

TEST(FilterColorMatrixProgramTests, WgslUnpremultipliesAndPremultipliesAroundTheMatrix) {
  const std::string wgsl = EmitFilterColorMatrixWgsl();

  // The matrix is defined on straight-alpha values, so the source is divided through by its alpha
  // and the clamped result is multiplied by its own alpha again. Dropping either would leave the
  // matrix applied to premultiplied values.
  EXPECT_THAT(wgsl, HasSubstr("(source.xyz / source.w)"));
  EXPECT_THAT(wgsl, HasSubstr("(clamped.xyz * clamped.w)"));
  // A fully transparent texel has no straight-alpha color to divide out, so it takes its own path
  // and returns rather than falling through to the matrix.
  EXPECT_THAT(wgsl, HasSubstr("} else {"));
  EXPECT_THAT(wgsl, HasSubstr("saturate(params.col4)"));
}

TEST(FilterColorMatrixProgramTests, MslDeclaresTheKernelSurface) {
  const std::string msl = EmitFilterColorMatrixMsl();

  EXPECT_THAT(msl, HasSubstr("kernel void cs_main("));
  EXPECT_THAT(msl, HasSubstr("uint3 gid [[thread_position_in_grid]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float> inputTexture [[texture(0)]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float, access::write> outputTexture [[texture(1)]]"));
  // A kernel takes its builtins directly, so no stage-in struct may appear.
  EXPECT_THAT(msl, testing::Not(HasSubstr("stage_in")));
}

TEST(FilterColorMatrixProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitFilterColorMatrixWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "filter_color_matrix.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("filter_color_matrix.wgsl")));
}

TEST(FilterColorMatrixProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitFilterColorMatrixMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "filter_color_matrix.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("filter_color_matrix.msl")));
}

TEST(FilterColorMatrixProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitFilterColorMatrixSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "filter_color_matrix.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("filter_color_matrix.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
