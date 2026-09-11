/// @file
/// Blend compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output. Native compiler and pixel tests validate execution.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Blend.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitBlendWgsl() {
  ShaderResult<IrModule> module = programs::BuildBlendModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitBlendMsl() {
  ShaderResult<IrModule> module = programs::BuildBlendModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::vector<uint32_t> EmitBlendSpirvWords() {
  ShaderResult<IrModule> module = programs::BuildBlendModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(BlendProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildBlendModule(), HasShaderResult());
}

TEST(BlendProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitBlendWgsl(), testing::Eq(EmitBlendWgsl()));
  EXPECT_THAT(EmitBlendMsl(), testing::Eq(EmitBlendMsl()));
  EXPECT_THAT(EmitBlendSpirvWords(), testing::Eq(EmitBlendSpirvWords()));
}

TEST(BlendProgramTests, DeclaresTwoSourcesFloatOutputAndUniformParameters) {
  const std::string wgsl = EmitBlendWgsl();
  EXPECT_THAT(wgsl, HasSubstr("var sourceTexture: texture_2d<f32>"));
  EXPECT_THAT(wgsl, HasSubstr("var destinationTexture: texture_2d<f32>"));
  EXPECT_THAT(wgsl, HasSubstr("texture_storage_2d<rgba32float, write>"));
  EXPECT_THAT(wgsl, HasSubstr("var<uniform> params: BlendParams"));
}

}  // namespace
}  // namespace donner::gpu::shader
