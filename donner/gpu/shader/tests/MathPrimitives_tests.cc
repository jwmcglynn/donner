/// @file
/// Emission tests for the `sign`, `floor`, and `pow` opcodes: the coverage module emits
/// deterministically in all three backends and matches its committed goldens byte-exactly, and
/// the round-half-away-from-zero composition the filter primitives need agrees with the rounding
/// the CPU filter path performs.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/shader/WgslEmitter.h"
#include "donner/gpu/shader/tests/MathPrimitiveCoverageModule.h"
#include "donner/gpu/shader/tests/ShaderGoldenUtils.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitMathPrimitiveWgsl() {
  ShaderResult<IrModule> module = BuildMathPrimitiveModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitWgsl(module.result()), std::string());
}

std::string EmitMathPrimitiveMsl() {
  ShaderResult<IrModule> module = BuildMathPrimitiveModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

std::string EmitMathPrimitiveSpirvBytes() {
  ShaderResult<IrModule> module = BuildMathPrimitiveModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return SpirvWordsToBytes(
      GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>()));
}

TEST(MathPrimitiveTests, ModuleBuildsCleanly) {
  EXPECT_THAT(BuildMathPrimitiveModule(), HasShaderResult());
}

TEST(MathPrimitiveTests, EmitsDeterministically) {
  EXPECT_THAT(EmitMathPrimitiveWgsl(), testing::Eq(EmitMathPrimitiveWgsl()));
  EXPECT_THAT(EmitMathPrimitiveMsl(), testing::Eq(EmitMathPrimitiveMsl()));
  EXPECT_THAT(EmitMathPrimitiveSpirvBytes(), testing::Eq(EmitMathPrimitiveSpirvBytes()));
}

TEST(MathPrimitiveTests, WgslSpellsScalarAndVectorFormsOfEachOpcode) {
  const std::string wgsl = EmitMathPrimitiveWgsl();

  EXPECT_THAT(wgsl, HasSubstr("return (sign(x) * floor((abs(x) + 0.5f)));"));
  EXPECT_THAT(wgsl, HasSubstr("let axisSigns = sign(axes);"));
  EXPECT_THAT(wgsl, HasSubstr("let axisFloors = floor(axes);"));
  EXPECT_THAT(wgsl, HasSubstr("let linearized = pow(((straight + 0.055f) / 1.055f), 2.4f);"));
  EXPECT_THAT(wgsl, HasSubstr("let curved = pow(vec2<f32>(straight, (1f - straight)), "
                              "vec2<f32>(2.4f, 2.4f));"));
}

TEST(MathPrimitiveTests, MslSpellsScalarAndVectorFormsOfEachOpcode) {
  const std::string msl = EmitMathPrimitiveMsl();

  // MSL names all three the same as WGSL, so the emitter routes them through the shared name
  // table rather than a special case; this is what fails if one gains a wrong special case.
  EXPECT_THAT(msl, HasSubstr("return (sign(x) * floor((abs(x) + 0.5f)));"));
  EXPECT_THAT(msl, HasSubstr("float2 axisSigns = sign(axes);"));
  EXPECT_THAT(msl, HasSubstr("float2 axisFloors = floor(axes);"));
  EXPECT_THAT(msl, HasSubstr("float linearized = pow(((straight + 0.055f) / 1.055f), 2.4f);"));
  EXPECT_THAT(msl, HasSubstr("float2 curved = pow(float2(straight, (1.0f - straight)), "
                             "float2(2.4f, 2.4f));"));
}

TEST(MathPrimitiveTests, RoundHalfAwayFromZeroMatchesTheCpuFilterPath) {
  // The CPU filter path rounds a pixel offset with std::round, which is round-half-away-from-zero.
  // WGSL's own round() is round-half-to-even, so the shader has to compose the rule out of sign
  // and floor. Pinning the composition against std::round over the table - negative halves
  // included - is what stops a program expressing the offset filter from re-deriving it and
  // landing on the banker's rounding the GPU builtin would give.
  bool sawADisagreementWithRoundHalfToEven = false;
  for (const float value : MathPrimitiveInputValues()) {
    EXPECT_FLOAT_EQ(RoundHalfAwayFromZeroOnHost(value), std::round(value))
        << "rounding of " << value << " diverges from the CPU filter path";
    if (RoundHalfAwayFromZeroOnHost(value) != std::nearbyint(value)) {
      sawADisagreementWithRoundHalfToEven = true;
    }
  }

  // Without a value where the two rules disagree the assertion above would pass for round() too,
  // and the composition would be pinned to nothing.
  EXPECT_TRUE(sawADisagreementWithRoundHalfToEven)
      << "the table has no half that separates away-from-zero from half-to-even rounding";
}

TEST(MathPrimitiveTests, WgslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_WGSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string wgsl = EmitMathPrimitiveWgsl();
  if (MaybeUpdateShaderGolden("UPDATE_WGSL_GOLDEN", "math_primitives.wgsl", wgsl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(wgsl, testing::Eq(ReadShaderGolden("math_primitives.wgsl")));
}

TEST(MathPrimitiveTests, MslMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitMathPrimitiveMsl();
  if (MaybeUpdateShaderGolden("UPDATE_MSL_GOLDEN", "math_primitives.msl", msl)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(msl, testing::Eq(ReadShaderGolden("math_primitives.msl")));
}

TEST(MathPrimitiveTests, SpirvMatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_SPIRV_GOLDEN=/path/to/repo rewrites the golden.
  const std::string bytes = EmitMathPrimitiveSpirvBytes();
  if (MaybeUpdateShaderGolden("UPDATE_SPIRV_GOLDEN", "math_primitives.spv", bytes)) {
    GTEST_SKIP() << "Golden updated";
  }
  EXPECT_THAT(bytes, testing::Eq(ReadShaderGolden("math_primitives.spv")));
}

}  // namespace
}  // namespace donner::gpu::shader
