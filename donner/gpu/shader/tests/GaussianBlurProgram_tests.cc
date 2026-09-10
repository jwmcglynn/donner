/// @file
/// GaussianBlur compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitGaussianBlurWgsl() {
  ShaderResult<IrModule> module = programs::BuildGaussianBlurModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitGaussianBlurMsl() {
  ShaderResult<IrModule> module = programs::BuildGaussianBlurModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitGaussianBlurSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildGaussianBlurModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(GaussianBlurProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildGaussianBlurModule(), HasShaderResult());
}

TEST(GaussianBlurProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitGaussianBlurWgsl(), testing::Eq(EmitGaussianBlurWgsl()));
  EXPECT_THAT(EmitGaussianBlurMsl(), testing::Eq(EmitGaussianBlurMsl()));
  EXPECT_THAT(EmitGaussianBlurSpirvBytes(), testing::Eq(EmitGaussianBlurSpirvBytes()));
}

TEST(GaussianBlurProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitGaussianBlurWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "gaussian_blur.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("gaussian_blur.wgsl")));
}

TEST(GaussianBlurProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitGaussianBlurMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "gaussian_blur.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("gaussian_blur.msl")));
}

TEST(GaussianBlurProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitGaussianBlurSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "gaussian_blur.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("gaussian_blur.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
