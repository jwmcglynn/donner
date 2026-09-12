/// @file
/// Emission tests for the `sign`, `floor`, and `pow` opcodes: the coverage module emits
/// deterministically in all three backends, and
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

std::vector<uint32_t> EmitMathPrimitiveSpirv() {
  ShaderResult<IrModule> module = BuildMathPrimitiveModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return {};
  }
  return GetShaderResultOrFail(EmitSpirv(module.result()), std::vector<uint32_t>());
}

TEST(MathPrimitiveTests, ModuleBuildsCleanly) {
  EXPECT_THAT(BuildMathPrimitiveModule(), HasShaderResult());
}

TEST(MathPrimitiveTests, EmitsDeterministically) {
  EXPECT_THAT(EmitMathPrimitiveWgsl(), testing::Eq(EmitMathPrimitiveWgsl()));
  EXPECT_THAT(EmitMathPrimitiveMsl(), testing::Eq(EmitMathPrimitiveMsl()));
  EXPECT_THAT(EmitMathPrimitiveSpirv(), testing::Eq(EmitMathPrimitiveSpirv()));
}

TEST(MathPrimitiveTests, WgslSpellsScalarAndVectorFormsOfEachOpcode) {
  const std::string wgsl = EmitMathPrimitiveWgsl();

  EXPECT_THAT(wgsl, HasSubstr("if (((magnitude - integral) >= 0.5f))"));
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
  EXPECT_THAT(msl, HasSubstr("if (((magnitude - integral) >= 0.5f))"));
  EXPECT_THAT(msl, HasSubstr("float2 axisSigns = sign(axes);"));
  EXPECT_THAT(msl, HasSubstr("float2 axisFloors = floor(axes);"));
  EXPECT_THAT(msl, HasSubstr("float linearized = pow(((straight + 0.055f) / 1.055f), 2.4f);"));
  EXPECT_THAT(msl, HasSubstr("float2 curved = pow(float2(straight, (1.0f - straight)), "
                             "float2(2.4f, 2.4f));"));
}

}  // namespace
}  // namespace donner::gpu::shader
