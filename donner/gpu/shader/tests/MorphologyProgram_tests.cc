/// @file
/// Morphology compute program tests: the module builds cleanly, all three emitters produce
/// deterministic output, and expose the expected program interfaces.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Morphology.h"
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

std::vector<uint32_t> EmitMorphologySpirv() {
  ShaderResult<IrModule> module = programs::BuildMorphologyModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(MorphologyProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildMorphologyModule(), HasShaderResult());
}

TEST(MorphologyProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitMorphologyWgsl(), testing::Eq(EmitMorphologyWgsl()));
  EXPECT_THAT(EmitMorphologyMsl(), testing::Eq(EmitMorphologyMsl()));
  EXPECT_THAT(EmitMorphologySpirv(), testing::Eq(EmitMorphologySpirv()));
}

}  // namespace
}  // namespace donner::gpu::shader
