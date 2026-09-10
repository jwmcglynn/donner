/// @file
/// ComponentTransfer compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and each matches its committed golden byte-exactly.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitComponentTransferWgsl() {
  ShaderResult<IrModule> module = programs::BuildComponentTransferModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitComponentTransferMsl() {
  ShaderResult<IrModule> module = programs::BuildComponentTransferModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitComponentTransferSpirvBytes() {
  ShaderResult<IrModule> module = programs::BuildComponentTransferModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(ComponentTransferProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildComponentTransferModule(), HasShaderResult());
}

TEST(ComponentTransferProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitComponentTransferWgsl(), testing::Eq(EmitComponentTransferWgsl()));
  EXPECT_THAT(EmitComponentTransferMsl(), testing::Eq(EmitComponentTransferMsl()));
  EXPECT_THAT(EmitComponentTransferSpirvBytes(), testing::Eq(EmitComponentTransferSpirvBytes()));
}

TEST(ComponentTransferProgramTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitComponentTransferWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "component_transfer.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("component_transfer.wgsl")));
}

TEST(ComponentTransferProgramTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitComponentTransferMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "component_transfer.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("component_transfer.msl")));
}

TEST(ComponentTransferProgramTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitComponentTransferSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "component_transfer.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("component_transfer.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
