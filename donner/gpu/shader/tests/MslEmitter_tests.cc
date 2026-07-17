/// @file
/// MSL emitter tests: determinism, the committed solid-fill golden, and fail-closed rejection of
/// layout divergence and reserved words.

#include "donner/gpu/shader/MslEmitter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "donner/base/tests/Runfiles.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

std::string EmitSolidFillMsl() {
  ShaderResult<IrModule> module = programs::BuildSolidFillModule();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    return "";
  }
  return GetShaderResultOrFail(EmitMsl(module.result()), std::string());
}

TEST(MslEmitterTests, EmitsDeterministically) {
  EXPECT_THAT(EmitSolidFillMsl(), testing::Eq(EmitSolidFillMsl()));
}

TEST(MslEmitterTests, OutputHasNoTrailingWhitespaceOrCr) {
  const std::string msl = EmitSolidFillMsl();
  EXPECT_THAT(msl, testing::Not(HasSubstr("\r")));
  EXPECT_THAT(msl, testing::Not(HasSubstr(" \n")));
}

TEST(MslEmitterTests, ContainsSolidFillSurface) {
  const std::string msl = EmitSolidFillMsl();

  EXPECT_THAT(msl, HasSubstr("vertex vs_main_Output vs_main(vs_main_Input in [[stage_in]], "
                             "uint instance_index [[instance_id]]"));
  EXPECT_THAT(msl, HasSubstr("fragment fs_main_Output fs_main(fs_main_Input in [[stage_in]]"));
  // The argument-table map from MslBindingMap.h: uniforms at buffer(1), vBandGrid at
  // buffer(12), textures/samplers at their binding indices.
  EXPECT_THAT(msl, HasSubstr("constant Uniforms& uniforms [[buffer(1)]]"));
  EXPECT_THAT(msl, HasSubstr("device const uint* vBandGrid [[buffer(12)]]"));
  EXPECT_THAT(msl, HasSubstr("texture2d<float> patternTexture [[texture(3)]]"));
  EXPECT_THAT(msl, HasSubstr("sampler patternSampler [[sampler(4)]]"));
  EXPECT_THAT(msl, HasSubstr("float4 clip_pos [[position]];"));
  EXPECT_THAT(msl, HasSubstr("float4 color [[color(0)]];"));
  EXPECT_THAT(msl, HasSubstr("discard_fragment();"));
  EXPECT_THAT(msl, HasSubstr("constant uint kNoBand = 4294967295u;"));
}

TEST(MslEmitterTests, MatchesCommittedGoldenByteExactly) {
  // Regenerate deliberately: UPDATE_MSL_GOLDEN=/path/to/repo rewrites the golden.
  const std::string msl = EmitSolidFillMsl();

  if (const char* updateRoot = std::getenv("UPDATE_MSL_GOLDEN")) {
    const std::string outPath =
        std::string(updateRoot) + "/donner/gpu/shader/tests/testdata/solid_fill.msl";
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "Failed to open " << outPath << " for writing";
    out << msl;
    GTEST_SKIP() << "Golden updated at " << outPath;
  }

  const std::string path =
      donner::Runfiles::instance().Rlocation("donner/gpu/shader/tests/testdata/solid_fill.msl");
  std::ifstream stream(path, std::ios::binary);
  ASSERT_TRUE(stream.good()) << "Failed to open golden file: " << path;
  std::ostringstream golden;
  golden << stream.rdbuf();

  EXPECT_THAT(msl, testing::Eq(golden.str()));
}

TEST(MslEmitterTests, RejectsUniformArrayStrideDivergence) {
  // array<f32, 4> in uniform has WGSL stride 16 but MSL C-array stride 4; the emitter must fail
  // closed rather than emit a mismatched layout.
  ModuleBuilder builder;
  const IrType arrayType =
      GetShaderResultOrFail(IrType::SizedArray(IrType::F32(), 4), IrType::F32());
  const IrType uniformType =
      GetShaderResultOrFail(IrType::Struct("BadUniforms", {{"taps", arrayType}}), IrType::F32());
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "badUniforms", uniformType), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()), IsShaderError(HasSubstr("diverges")));
}

TEST(MslEmitterTests, RejectsVec3OffsetDivergence) {
  // MSL float3 occupies 16 bytes; WGSL vec3<f32> is 12 with the follower packed at offset 12.
  ModuleBuilder builder;
  const IrType structType = GetShaderResultOrFail(
      IrType::Struct("Padded", {{"a", IrType::Vec3f()}, {"b", IrType::F32()}}), IrType::F32());
  EXPECT_THAT(builder.addReadOnlyStorageBuffer(0, 0, "padded", structType), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()),
              IsShaderError(HasSubstr("diverges from the WGSL layout offset")));
}

TEST(MslEmitterTests, RejectsSyntacticallyInvalidIdentifiers) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addConstant("bad name", LiteralU32(1)), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()), IsShaderError(HasSubstr("not a valid identifier")));
}

TEST(MslEmitterTests, RejectsLeadingDoubleUnderscoreIdentifiers) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addConstant("__reserved", LiteralU32(1)), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()), IsShaderError(HasSubstr("not a valid identifier")));
}

TEST(MslEmitterTests, RejectsNonHostShareableUniformStructs) {
  // Covered by the shared WGSL layout verification the MSL emitter already runs on binding
  // roots; pinned here so the coverage cannot regress.
  ModuleBuilder builder;
  const IrType structType =
      GetShaderResultOrFail(IrType::Struct("Flags", {{"flag", IrType::Bool()}}), IrType::F32());
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "flags", structType), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()), IsShaderError(HasSubstr("not host-shareable")));
}

TEST(MslEmitterTests, RejectsUserStructCollidingWithGeneratedIoStructs) {
  ModuleBuilder builder;
  const IrType collidingType =
      GetShaderResultOrFail(IrType::Struct("vs_main_Input", {{"x", IrType::F32()}}), IrType::F32());
  EXPECT_THAT(builder.addReadOnlyStorageBuffer(0, 0, "colliding", collidingType), IsShaderOk());

  auto vertex = builder.createVertexEntryPoint(
      "vs_main", {IrParam{"pos", IrType::Vec2f(), 0}},
      {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position}});
  ASSERT_THAT(vertex, HasShaderResult());
  FunctionBuilder function = std::move(vertex).result();
  EXPECT_THAT(function.returnOutputs({GetShaderResultOrFail(
                  ConstructVector(IrType::Vec4f(), {LiteralF32(0.0f)}), LiteralF32(0))}),
              IsShaderOk());
  EXPECT_THAT(function.finish(), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()),
              IsShaderError(HasSubstr("collides with a generated stage IO struct")));
}

TEST(MslEmitterTests, RejectsBufferBindingsCollidingWithVertexBufferIndex) {
  // Buffer binding 29 maps to Metal buffer index 30, the reserved stage-in vertex buffer slot;
  // bindings 30 and 31 would exceed Metal's 0..30 argument table.
  ModuleBuilder builder;
  const IrType structType =
      GetShaderResultOrFail(IrType::Struct("Params", {{"x", IrType::F32()}}), IrType::F32());
  EXPECT_THAT(builder.addUniformBuffer(0, 29, "params", structType), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()),
              IsShaderError(HasSubstr("collides with or exceeds the reserved stage-in vertex "
                                      "buffer index")));
}

TEST(MslEmitterTests, RejectsMslReservedWords) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addConstant("device", LiteralU32(1)), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()),
              IsShaderError(HasSubstr("collides with an MSL reserved word")));
}

}  // namespace
}  // namespace donner::gpu::shader
