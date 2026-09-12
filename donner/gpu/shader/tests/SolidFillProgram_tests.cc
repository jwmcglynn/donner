/// @file
/// Solid-fill program tests cover construction, deterministic emission, and its binding surface.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <format>
#include <string>

#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitSolidFill() {
  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

TEST(SolidFillProgramTests, ModuleBuildsCleanly) {
  EXPECT_THAT(programs::BuildSolidFillModule(), HasShaderResult());
}

TEST(SolidFillProgramTests, EmitsDeterministically) {
  EXPECT_THAT(EmitSolidFill(), testing::Eq(EmitSolidFill()));
}

TEST(SolidFillProgramTests, ContainsSlugFillSurface) {
  const std::string wgsl = EmitSolidFill();

  // Entry points and every binding index of the slug fill pipeline.
  EXPECT_THAT(wgsl, HasSubstr("@vertex\nfn vs_main("));
  EXPECT_THAT(wgsl, HasSubstr("@fragment\nfn fs_main("));
  for (int binding = 0; binding <= 11; ++binding) {
    EXPECT_THAT(wgsl, HasSubstr(std::format("@binding({}) ", binding)));
  }
  EXPECT_THAT(wgsl, HasSubstr("const kNoBand: u32 = 4294967295u;"));
  EXPECT_THAT(wgsl, HasSubstr("discard;"));
}

}  // namespace
}  // namespace donner::gpu::shader
