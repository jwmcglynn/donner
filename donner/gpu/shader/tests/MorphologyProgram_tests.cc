/// @file
/// Morphology compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitMorphologyWgsl() {
  ShaderResult<IrModule> module = programs::BuildMorphologyModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitMorphologyMsl() {
  ShaderResult<IrModule> module = programs::BuildMorphologyModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitMorphologySpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildMorphologyModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(MorphologyProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildMorphologyModule(), HasShaderResult());
}

TEST(MorphologyProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitMorphologyWgsl(), testing::Eq(EmitMorphologyWgsl()));
  EXPECT_THAT(EmitMorphologyMsl(), testing::Eq(EmitMorphologyMsl()));
  EXPECT_THAT(EmitMorphologySpirvBytes(), testing::Eq(EmitMorphologySpirvBytes()));
}

TEST(MorphologyProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitMorphologyWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "morphology.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("morphology.wgsl")));
}

TEST(MorphologyProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitMorphologyMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "morphology.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("morphology.msl")));
}

TEST(MorphologyProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitMorphologySpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "morphology.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("morphology.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
