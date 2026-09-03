/// @file
/// MSL emitter tests: determinism, the committed solid-fill golden, and fail-closed rejection of
/// layout divergence and reserved words.

#include "donner/gpu/shader/MslEmitter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "donner/base/tests/Runfiles.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/ReductionCoverageModule.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"
#include "donner/gpu/shader/tests/StageIoTestModules.h"

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

TEST(MslEmitterTests, BothBoolVectorReductionsEmitTheirMslSpellings) {
  ShaderResult<IrModule> module = BuildVectorReductionModule();
  ASSERT_THAT(module, HasShaderResult());
  const std::string msl = GetShaderResultOrFail(EmitMsl(module.result()), std::string());

  // Metal spells the reductions the same way WGSL does, so a wrong mapping shows up as the other
  // reduction's name rather than as a compile error the validator would catch for us.
  EXPECT_THAT(msl, testing::HasSubstr("const bool inside = all((gid.xy < extent));"));
  EXPECT_THAT(msl, testing::HasSubstr("const bool overflow = any((gid.xy >= extent));"));
}

TEST(MslEmitterTests, EmitsDeterministically) {
  EXPECT_THAT(EmitSolidFillMsl(), testing::Eq(EmitSolidFillMsl()));
}

TEST(MslEmitterTests, OutputHasNoTrailingWhitespaceOrCr) {
  const std::string msl = EmitSolidFillMsl();
  EXPECT_THAT(msl, testing::Not(HasSubstr("\r")));
  EXPECT_THAT(msl, testing::Not(HasSubstr(" \n")));
}

TEST(MslEmitterTests, APositionOnlyFragmentEntryStillCarriesItsInput) {
  ShaderResult<IrModule> module = BuildPositionOnlyFragmentModule();
  ASSERT_THAT(module, HasShaderResult());
  ShaderResult<std::string> msl = EmitMsl(module.result());
  ASSERT_THAT(msl, HasShaderResult());

  // The stage-in struct is omitted only when nothing would go in it. Position has no
  // direct-parameter spelling here, so this entry keeps its struct; dropping it would leave the
  // body referencing an input that no parameter carries.
  EXPECT_THAT(msl.result(), HasSubstr("struct fs_position_only_Input {"));
  EXPECT_THAT(msl.result(), HasSubstr("float4 frag_pos [[position]];"));
  EXPECT_THAT(msl.result(), HasSubstr("fragment fs_position_only_Output fs_position_only("
                                      "fs_position_only_Input in [[stage_in]]"));
  EXPECT_THAT(msl.result(), HasSubstr("const float4 frag_pos = in.frag_pos;"));
}

TEST(MslEmitterTests, ContainsSolidFillSurface) {
  const std::string msl = EmitSolidFillMsl();

  // The vertex stage builds its geometry from vertex_index, so it takes both builtins as
  // direct parameters and declares no (empty, and therefore invalid) [[stage_in]] struct.
  EXPECT_THAT(msl, HasSubstr("vertex vs_main_Output vs_main(uint vertex_index [[vertex_id]], "
                             "uint instance_index [[instance_id]]"));
  EXPECT_THAT(msl, testing::Not(HasSubstr("struct vs_main_Input")));
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

TEST(MslEmitterTests, RejectsBindingsOutsideGroupZero) {
  // The flat Metal argument-table map models bind group 0 only; a binding in another group
  // would silently collide with group 0's indices.
  ModuleBuilder builder;
  const IrType structType =
      GetShaderResultOrFail(IrType::Struct("Params", {{"x", IrType::F32()}}), IrType::F32());
  EXPECT_THAT(builder.addUniformBuffer(1, 0, "params", structType), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()), IsShaderError(HasSubstr("only bind group 0")));
}

TEST(MslEmitterTests, RejectsLocalsShadowingBindingNames) {
  // Bindings become implicit MSL parameters on every function; a local with the same name
  // would shadow the parameter, and user-call forwarding would then pass the local.
  ModuleBuilder builder;
  const IrType structType =
      GetShaderResultOrFail(IrType::Struct("Params", {{"x", IrType::F32()}}), IrType::F32());
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "params", structType), IsShaderOk());

  auto fn = builder.createFunction("helper", {}, IrType::F32());
  ASSERT_THAT(fn, HasShaderResult());
  FunctionBuilder function = std::move(fn).result();
  EXPECT_THAT(function.addVar("params", IrType::F32(), LiteralF32(0.0f)), HasShaderResult());
  EXPECT_THAT(function.returnValue(LiteralF32(1.0f)), IsShaderOk());
  EXPECT_THAT(function.finish(), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()), IsShaderError(HasSubstr("shadows an implicit MSL")));
}

TEST(MslEmitterTests, RejectsEntryLocalsShadowingStageInName) {
  // Entry points receive the generated stage-in struct as a parameter named `in`; a user local
  // with that name would shadow it.
  ModuleBuilder builder;
  auto vertex = builder.createVertexEntryPoint(
      "vs_main", {IrParam{"pos", IrType::Vec2f(), 0}},
      {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position}});
  ASSERT_THAT(vertex, HasShaderResult());
  FunctionBuilder function = std::move(vertex).result();
  EXPECT_THAT(function.addLet("in", LiteralF32(1.0f)), HasShaderResult());
  EXPECT_THAT(function.returnOutputs({GetShaderResultOrFail(
                  ConstructVector(IrType::Vec4f(), {LiteralF32(0.0f)}), LiteralF32(0))}),
              IsShaderOk());
  EXPECT_THAT(function.finish(), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()), IsShaderError(HasSubstr("shadows an implicit MSL")));
}

TEST(MslEmitterTests, RejectsALocalNamedAfterACalledBuiltin) {
  // The reductions are emitted as free function calls in MSL too, so a local named after one
  // shadows it exactly as it would in WGSL.
  ModuleBuilder builder;
  {
    ShaderResult<FunctionBuilder> fnResult =
        builder.createFunction("shadow", {IrParam{"v", IrType::Vec2u()}}, IrType::Bool());
    ASSERT_THAT(fnResult, HasShaderResult());
    FunctionBuilder fn = std::move(fnResult).result();
    const IrExpr v = GetShaderResultOrFail(fn.ref("v"), LiteralF32(0));
    const IrExpr reduced = GetShaderResultOrFail(
        CallBuiltin(BuiltinFn::Any, {GetShaderResultOrFail(Ge(v, v), LiteralF32(0))}),
        LiteralF32(0));
    EXPECT_THAT(fn.addLet("any", reduced), HasShaderResult());
    EXPECT_THAT(fn.returnValue(GetShaderResultOrFail(fn.ref("any"), LiteralF32(0))), IsShaderOk());
    EXPECT_THAT(fn.finish(), IsShaderOk());
  }

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitMsl(module.result()),
              IsShaderError(HasSubstr("collides with an MSL reserved word")));
}

TEST(MslEmitterTests, EveryFreeFunctionBuiltinNameIsReserved) {
  // The WGSL emitter has the same guard, and MSL needs its own because the two reserved-word
  // tables are separate. Four builtins are excluded because MSL does not spell them as a free
  // function call: `select` becomes a ternary and the three texture builtins become methods on
  // the texture object, so their WGSL names are never emitted here. Anything else the enum
  // grows is emitted by name and must therefore be unavailable as an identifier; a builtin
  // added without its reserved word fails here rather than in a shader that shadows it.
  const std::array<BuiltinFn, 4> notSpelledAsACall = {BuiltinFn::Select, BuiltinFn::TextureSample,
                                                      BuiltinFn::TextureLoad,
                                                      BuiltinFn::TextureDimensions};

  for (int raw = 0; raw < 256; ++raw) {
    const BuiltinFn builtin = static_cast<BuiltinFn>(raw);
    std::ostringstream name;
    name << builtin;
    if (name.str() == "unknown") {
      EXPECT_GT(raw, 0) << "the builtin enum walk found no builtins at all";
      break;
    }
    if (std::find(notSpelledAsACall.begin(), notSpelledAsACall.end(), builtin) !=
        notSpelledAsACall.end()) {
      continue;
    }

    ModuleBuilder builder;
    ASSERT_THAT(builder.addConstant(RcString(name.str()), LiteralU32(1)), IsShaderOk());
    ShaderResult<IrModule> module = builder.build();
    ASSERT_THAT(module, HasShaderResult());
    EXPECT_THAT(EmitMsl(module.result()),
                IsShaderError(HasSubstr("collides with an MSL reserved word")))
        << "builtin \"" << name.str() << "\" is spelled as a call but its name is not reserved";
  }
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
