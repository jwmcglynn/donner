/// @file
/// GaussianBlur compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, with compiler and execution checks in separate suites.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

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

std::vector<uint32_t> EmitGaussianBlurSpirvWords() {
  ShaderResult<IrModule> module = programs::BuildGaussianBlurModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(GaussianBlurProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildGaussianBlurModule(), HasShaderResult());
}

TEST(GaussianBlurProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitGaussianBlurWgsl(), testing::Eq(EmitGaussianBlurWgsl()));
  EXPECT_THAT(EmitGaussianBlurMsl(), testing::Eq(EmitGaussianBlurMsl()));
  EXPECT_THAT(EmitGaussianBlurSpirvWords(), testing::Eq(EmitGaussianBlurSpirvWords()));
}

}  // namespace
}  // namespace donner::gpu::shader
