/// @file
/// ComponentTransfer compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output. Native compiler and pixel tests validate execution.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/ComponentTransfer.h"
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

std::vector<uint32_t> EmitComponentTransferSpirvWords() {
  ShaderResult<IrModule> module = programs::BuildComponentTransferModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(ComponentTransferProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildComponentTransferModule(), HasShaderResult());
}

TEST(ComponentTransferProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitComponentTransferWgsl(), testing::Eq(EmitComponentTransferWgsl()));
  EXPECT_THAT(EmitComponentTransferMsl(), testing::Eq(EmitComponentTransferMsl()));
  EXPECT_THAT(EmitComponentTransferSpirvWords(), testing::Eq(EmitComponentTransferSpirvWords()));
}

TEST(ComponentTransferProgramTests, UsesPackedReadOnlyStorageAndFloatOutput) {
  const std::string wgsl = EmitComponentTransferWgsl();
  EXPECT_THAT(wgsl, HasSubstr("var<storage, read> params: array<f32>"));
  EXPECT_THAT(wgsl, HasSubstr("texture_storage_2d<rgba32float, write>"));
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("ComponentTransferParams")));
}

}  // namespace
}  // namespace donner::gpu::shader
