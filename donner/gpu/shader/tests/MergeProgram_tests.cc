#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Merge.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

namespace donner::gpu::shader {
namespace {

TEST(MergeProgramTests, BuildsAndEmitsDeterministically) {
  const auto module = programs::BuildMergeModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto wgsl = EmitWgsl(module.result());
  const auto repeatedWgsl = EmitWgsl(module.result());
  ASSERT_THAT(wgsl, HasShaderResult());
  ASSERT_THAT(repeatedWgsl, HasShaderResult());
  EXPECT_THAT(repeatedWgsl.result(), testing::Eq(wgsl.result()));
  const auto msl = EmitMsl(module.result());
  const auto repeatedMsl = EmitMsl(module.result());
  ASSERT_THAT(msl, HasShaderResult());
  ASSERT_THAT(repeatedMsl, HasShaderResult());
  EXPECT_THAT(repeatedMsl.result(), testing::Eq(msl.result()));
  const auto spirv = EmitSpirv(module.result());
  const auto repeatedSpirv = EmitSpirv(module.result());
  ASSERT_THAT(spirv, HasShaderResult());
  ASSERT_THAT(repeatedSpirv, HasShaderResult());
  EXPECT_THAT(repeatedSpirv.result(), testing::Eq(spirv.result()));
}

TEST(MergeProgramTests, GuardsRoundedDispatchAndClampsPremultipliedResult) {
  const auto module = programs::BuildMergeModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto wgsl = EmitWgsl(module.result());
  ASSERT_THAT(wgsl, HasShaderResult());
  EXPECT_THAT(wgsl.result(), testing::HasSubstr("@compute @workgroup_size(8, 8, 1)"));
  EXPECT_THAT(wgsl.result(),
              testing::HasSubstr(
                  "let extent = textureDimensions(outputTexture);\n"
                  "  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {\n    return;\n  }"));
  EXPECT_THAT(wgsl.result(), testing::HasSubstr("source + (destination * remainingSourceAlpha)"));
  EXPECT_THAT(wgsl.result(),
              testing::HasSubstr("textureStore(outputTexture, coords, saturate(result));"));
}

}  // namespace
}  // namespace donner::gpu::shader
