/// @file
/// DropShadow compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/DropShadow.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitDropShadowWgsl() {
  ShaderResult<IrModule> module = programs::BuildDropShadowModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitDropShadowMsl() {
  ShaderResult<IrModule> module = programs::BuildDropShadowModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitDropShadowSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildDropShadowModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(DropShadowProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildDropShadowModule(), HasShaderResult());
}

TEST(DropShadowProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitDropShadowWgsl(), testing::Eq(EmitDropShadowWgsl()));
  EXPECT_THAT(EmitDropShadowMsl(), testing::Eq(EmitDropShadowMsl()));
  EXPECT_THAT(EmitDropShadowSpirvBytes(), testing::Eq(EmitDropShadowSpirvBytes()));
}

TEST(DropShadowProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitDropShadowWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "drop_shadow.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("drop_shadow.wgsl")));
}

TEST(DropShadowProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitDropShadowMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "drop_shadow.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("drop_shadow.msl")));
}

TEST(DropShadowProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitDropShadowSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "drop_shadow.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("drop_shadow.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
