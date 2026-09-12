#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/programs/Composite.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

namespace donner::gpu::shader {
namespace {

TEST(CompositeProgramTests, BuildsAndEmitsDeterministically) {
  const auto module = programs::BuildCompositeModule();
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

TEST(CompositeProgramTests, GuardsRoundedDispatchAndClampsPremultipliedResult) {
  const auto module = programs::BuildCompositeModule();
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

TEST(CompositeProgramTests, SelectsEachPorterDuffOperatorAndAllArithmeticCoefficients) {
  const auto module = programs::BuildCompositeModule();
  ASSERT_THAT(module, HasShaderResult());
  const auto wgsl = EmitWgsl(module.result());
  ASSERT_THAT(wgsl, HasShaderResult());
  for (uint32_t op = 1; op <= 6; ++op) {
    EXPECT_THAT(wgsl.result(),
                testing::HasSubstr("if ((params.op == " + std::to_string(op) + "u))"));
  }
  EXPECT_THAT(wgsl.result(), testing::HasSubstr("result = (source * destination.w);"));
  EXPECT_THAT(wgsl.result(), testing::HasSubstr("result = (source * remainingDestinationAlpha);"));
  EXPECT_THAT(wgsl.result(),
              testing::HasSubstr(
                  "result = ((source * destination.w) + (destination * remainingSourceAlpha));"));
  EXPECT_THAT(wgsl.result(), testing::HasSubstr("result = ((source * remainingDestinationAlpha) + "
                                                "(destination * remainingSourceAlpha));"));
  EXPECT_THAT(wgsl.result(), testing::HasSubstr("result = (source + destination);"));
  EXPECT_THAT(wgsl.result(),
              testing::HasSubstr(
                  "result = (((((params.k1 * source) * destination) + (params.k2 * source)) + "
                  "(params.k3 * destination)) + vec4<f32>(params.k4));"));
}

}  // namespace
}  // namespace donner::gpu::shader
