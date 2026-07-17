/// @file
/// WGSL emitter tests: a per-construct golden module, determinism, and fail-closed rejection of
/// reserved words, padded uniform arrays, and non-finite literals.

#include "donner/gpu/shader/WgslEmitter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

/// Builds a compact module exercising every emitted node kind: structs, a constant, all four
/// binding kinds, a plain function (var/for/if/continue/index/member arithmetic), and vertex +
/// fragment entry points (builtins, constructors, conversions, texture builtins, select,
/// logical/unary ops, swizzle assignment, discard).
IrModule BuildEmitterCoverageModule() {
  ModuleBuilder builder;

  const IrType paramsType = GetShaderResultOrFail(
      IrType::Struct("Params", {{"scale", IrType::Vec2f()}, {"count", IrType::U32()}}),
      IrType::F32());
  const IrType valuesType =
      GetShaderResultOrFail(IrType::RuntimeArray(IrType::F32()), IrType::F32());

  EXPECT_THAT(builder.addConstant("kSentinel", LiteralU32(0xFFFFFFFFu)), IsShaderOk());
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "params", paramsType), IsShaderOk());
  EXPECT_THAT(builder.addReadOnlyStorageBuffer(0, 1, "values", valuesType), IsShaderOk());
  EXPECT_THAT(builder.addTexture2d(0, 2, "tex"), IsShaderOk());
  EXPECT_THAT(builder.addSampler(0, 3, "smp"), IsShaderOk());

  {
    auto result =
        builder.createFunction("sum_values", {IrParam{"limit", IrType::U32()}}, IrType::F32());
    EXPECT_THAT(result, HasShaderResult());
    FunctionBuilder fn = std::move(result).result();

    const IrExpr limit = GetShaderResultOrFail(fn.ref("limit"), LiteralF32(0));
    const IrExpr values = GetShaderResultOrFail(fn.ref("values"), LiteralF32(0));
    const IrExpr total =
        GetShaderResultOrFail(fn.addVar("total", IrType::F32(), LiteralF32(0.0f)), LiteralF32(0));

    const IrExpr i = GetShaderResultOrFail(fn.beginFor("i", LiteralU32(0)), LiteralF32(0));
    EXPECT_THAT(fn.forCondition(GetShaderResultOrFail(Lt(i, limit), LiteralF32(0))), IsShaderOk());
    EXPECT_THAT(fn.forContinuing(i, GetShaderResultOrFail(Add(i, LiteralU32(1)), LiteralF32(0))),
                IsShaderOk());
    const IrExpr element = GetShaderResultOrFail(Index(values, i), LiteralF32(0));
    EXPECT_THAT(fn.beginIf(GetShaderResultOrFail(Lt(element, LiteralF32(0.0f)), LiteralF32(0))),
                IsShaderOk());
    EXPECT_THAT(fn.continueStmt(), IsShaderOk());
    EXPECT_THAT(fn.endIf(), IsShaderOk());
    EXPECT_THAT(fn.assign(total, GetShaderResultOrFail(Add(total, element), LiteralF32(0))),
                IsShaderOk());
    EXPECT_THAT(fn.endFor(), IsShaderOk());
    EXPECT_THAT(fn.returnValue(total), IsShaderOk());
    EXPECT_THAT(fn.finish(), IsShaderOk());
  }

  {
    auto result = builder.createVertexEntryPoint(
        "vs_test",
        {IrParam{"instance_index", IrType::U32(), std::nullopt, BuiltinInput::InstanceIndex},
         IrParam{"pos", IrType::Vec2f(), 0}},
        {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position},
         IrOutputMember{"uv", IrType::Vec2f(), 0}});
    EXPECT_THAT(result, HasShaderResult());
    FunctionBuilder fn = std::move(result).result();

    const IrExpr pos = GetShaderResultOrFail(fn.ref("pos"), LiteralF32(0));
    const IrExpr params = GetShaderResultOrFail(fn.ref("params"), LiteralF32(0));
    const IrExpr scaled = GetShaderResultOrFail(
        fn.addLet("scaled",
                  GetShaderResultOrFail(
                      Mul(pos, GetShaderResultOrFail(Member(params, "scale"), LiteralF32(0))),
                      LiteralF32(0))),
        LiteralF32(0));
    const IrExpr clipPos = GetShaderResultOrFail(
        ConstructVector(IrType::Vec4f(), {scaled, LiteralF32(0.0f), LiteralF32(1.0f)}),
        LiteralF32(0));
    EXPECT_THAT(fn.returnOutputs({clipPos, scaled}), IsShaderOk());
    EXPECT_THAT(fn.finish(), IsShaderOk());
  }

  {
    auto result = builder.createFragmentEntryPoint(
        "fs_test",
        {IrParam{"frag_pos", IrType::Vec4f(), std::nullopt, BuiltinInput::Position},
         IrParam{"uv", IrType::Vec2f(), 0}},
        {IrOutputMember{"color", IrType::Vec4f(), 0}});
    EXPECT_THAT(result, HasShaderResult());
    FunctionBuilder fn = std::move(result).result();

    const IrExpr uv = GetShaderResultOrFail(fn.ref("uv"), LiteralF32(0));
    const IrExpr tex = GetShaderResultOrFail(fn.ref("tex"), LiteralF32(0));
    const IrExpr smp = GetShaderResultOrFail(fn.ref("smp"), LiteralF32(0));

    const IrExpr texel = GetShaderResultOrFail(
        fn.addLet("texel",
                  GetShaderResultOrFail(CallBuiltin(BuiltinFn::TextureSample, {tex, smp, uv}),
                                        LiteralF32(0))),
        LiteralF32(0));
    const IrExpr dims = GetShaderResultOrFail(
        fn.addLet("dims",
                  GetShaderResultOrFail(
                      Convert(IrType::Vec2i(),
                              GetShaderResultOrFail(
                                  CallBuiltin(BuiltinFn::TextureDimensions, {tex}), LiteralF32(0))),
                      LiteralF32(0))),
        LiteralF32(0));
    const IrExpr loaded = GetShaderResultOrFail(
        fn.addLet(
            "loaded",
            GetShaderResultOrFail(
                CallBuiltin(
                    BuiltinFn::TextureLoad,
                    {tex,
                     GetShaderResultOrFail(
                         CallBuiltin(
                             BuiltinFn::Clamp,
                             {GetShaderResultOrFail(
                                  Convert(IrType::Vec2i(),
                                          GetShaderResultOrFail(CallBuiltin(BuiltinFn::Round, {uv}),
                                                                LiteralF32(0))),
                                  LiteralF32(0)),
                              GetShaderResultOrFail(
                                  ConstructVector(IrType::Vec2i(), {LiteralI32(0)}), LiteralF32(0)),
                              GetShaderResultOrFail(
                                  Sub(dims, GetShaderResultOrFail(
                                                ConstructVector(IrType::Vec2i(), {LiteralI32(1)}),
                                                LiteralF32(0))),
                                  LiteralF32(0))}),
                         LiteralF32(0)),
                     LiteralI32(0)}),
                LiteralF32(0))),
        LiteralF32(0));

    const IrExpr total = GetShaderResultOrFail(
        fn.addLet("total",
                  GetShaderResultOrFail(
                      fn.callFunction("sum_values",
                                      {GetShaderResultOrFail(fn.ref("kSentinel"), LiteralF32(0))}),
                      LiteralF32(0))),
        LiteralF32(0));
    const IrExpr flag = GetShaderResultOrFail(
        fn.addLet("flag", GetShaderResultOrFail(
                              And(GetShaderResultOrFail(Gt(total, LiteralF32(0.0f)), LiteralF32(0)),
                                  GetShaderResultOrFail(
                                      Not(GetShaderResultOrFail(
                                          Lt(GetShaderResultOrFail(Swizzle(uv, "x"), LiteralF32(0)),
                                             LiteralF32(0.0f)),
                                          LiteralF32(0))),
                                      LiteralF32(0))),
                              LiteralF32(0))),
        LiteralF32(0));

    const IrExpr color = GetShaderResultOrFail(
        fn.addVar("color", IrType::Vec4f(),
                  GetShaderResultOrFail(CallBuiltin(BuiltinFn::Select, {texel, loaded, flag}),
                                        LiteralF32(0))),
        LiteralF32(0));
    EXPECT_THAT(fn.assign(GetShaderResultOrFail(Swizzle(color, "w"), LiteralF32(0)),
                          GetShaderResultOrFail(Neg(total), LiteralF32(0))),
                IsShaderOk());

    EXPECT_THAT(fn.beginIf(flag), IsShaderOk());
    EXPECT_THAT(fn.discard(), IsShaderOk());
    EXPECT_THAT(fn.endIf(), IsShaderOk());

    EXPECT_THAT(fn.returnOutputs({color}), IsShaderOk());
    EXPECT_THAT(fn.finish(), IsShaderOk());
  }

  ShaderResult<IrModule> module = builder.build();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    ModuleBuilder emptyBuilder;
    return std::move(emptyBuilder.build()).result();
  }
  return std::move(module).result();
}

TEST(WgslEmitterTests, EmitsDeterministically) {
  const std::string first =
      GetShaderResultOrFail(EmitWgsl(BuildEmitterCoverageModule()), std::string());
  const std::string second =
      GetShaderResultOrFail(EmitWgsl(BuildEmitterCoverageModule()), std::string());
  EXPECT_THAT(first, testing::Eq(second));
}

TEST(WgslEmitterTests, OutputHasNoTrailingWhitespaceOrCr) {
  const std::string wgsl =
      GetShaderResultOrFail(EmitWgsl(BuildEmitterCoverageModule()), std::string());
  EXPECT_THAT(wgsl, testing::Not(HasSubstr("\r")));
  EXPECT_THAT(wgsl, testing::Not(HasSubstr(" \n")));
}

TEST(WgslEmitterTests, GoldenEmissionOfCoverageModule) {
  const std::string wgsl =
      GetShaderResultOrFail(EmitWgsl(BuildEmitterCoverageModule()), std::string());

  // clang-format off
  const std::string kExpected = R"wgsl(// Generated by donner::gpu::shader::WgslEmitter. Do not edit.

struct Params {
  scale: vec2<f32>,
  count: u32,
}

const kSentinel: u32 = 4294967295u;

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> values: array<f32>;
@group(0) @binding(2) var tex: texture_2d<f32>;
@group(0) @binding(3) var smp: sampler;

fn sum_values(limit: u32) -> f32 {
  var total: f32 = 0f;
  for (var i: u32 = 0u; (i < limit); i = (i + 1u)) {
    if ((values[i] < 0f)) {
      continue;
    }
    total = (total + values[i]);
  }
  return total;
}

struct vs_test_Output {
  @builtin(position) clip_pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
}

@vertex
fn vs_test(@builtin(instance_index) instance_index: u32, @location(0) pos: vec2<f32>) -> vs_test_Output {
  let scaled = (pos * params.scale);
  return vs_test_Output(vec4<f32>(scaled, 0f, 1f), scaled);
}

struct fs_test_Output {
  @location(0) color: vec4<f32>,
}

@fragment
fn fs_test(@builtin(position) frag_pos: vec4<f32>, @location(0) uv: vec2<f32>) -> fs_test_Output {
  let texel = textureSample(tex, smp, uv);
  let dims = vec2<i32>(textureDimensions(tex));
  let loaded = textureLoad(tex, clamp(vec2<i32>(round(uv)), vec2<i32>(0i), (dims - vec2<i32>(1i))), 0i);
  let total = sum_values(kSentinel);
  let flag = ((total > 0f) && (!(uv.x < 0f)));
  var color: vec4<f32> = select(texel, loaded, flag);
  color.w = (-total);
  if (flag) {
    discard;
  }
  return fs_test_Output(color);
}
)wgsl";
  // clang-format on

  EXPECT_THAT(wgsl, testing::Eq(kExpected));
}

TEST(WgslEmitterTests, CollectsStructTypedForInitVariables) {
  // For-loop init variables live in IrStmt::Data::init rather than the body block; a
  // struct-typed loop variable must still produce a struct declaration (exactly once, before
  // its first use).
  ModuleBuilder builder;
  const IrType cursorType =
      GetShaderResultOrFail(IrType::Struct("Cursor", {{"position", IrType::U32()}}), IrType::F32());

  {
    auto helper = builder.createFunction("makeCursor", {}, cursorType);
    ASSERT_THAT(helper, HasShaderResult());
    FunctionBuilder fn = std::move(helper).result();
    const IrExpr cursor = GetShaderResultOrFail(fn.addVar("cursor", cursorType), LiteralF32(0));
    EXPECT_THAT(
        fn.assign(GetShaderResultOrFail(Member(cursor, "position"), LiteralF32(0)), LiteralU32(0)),
        IsShaderOk());
    EXPECT_THAT(fn.returnValue(cursor), IsShaderOk());
    EXPECT_THAT(fn.finish(), IsShaderOk());
  }
  {
    auto walker = builder.createFunction("walk", {}, std::nullopt);
    ASSERT_THAT(walker, HasShaderResult());
    FunctionBuilder fn = std::move(walker).result();
    const IrExpr c = GetShaderResultOrFail(
        fn.beginFor("c", GetShaderResultOrFail(fn.callFunction("makeCursor", {}), LiteralF32(0))),
        LiteralF32(0));
    EXPECT_THAT(fn.forCondition(GetShaderResultOrFail(
                    Lt(GetShaderResultOrFail(Member(c, "position"), LiteralF32(0)), LiteralU32(4)),
                    LiteralF32(0))),
                IsShaderOk());
    EXPECT_THAT(fn.endFor(), IsShaderOk());
    EXPECT_THAT(fn.finish(), IsShaderOk());
  }

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  const std::string wgsl = GetShaderResultOrFail(EmitWgsl(module.result()), std::string());

  EXPECT_THAT(wgsl, HasSubstr("struct Cursor {"));
  EXPECT_THAT(wgsl, HasSubstr("for (var c: Cursor = makeCursor();"));
  // Declared exactly once, before its first use.
  EXPECT_THAT(wgsl.find("struct Cursor {"), testing::Lt(wgsl.find("fn makeCursor")));
  EXPECT_THAT(wgsl.find("struct Cursor {"), testing::Eq(wgsl.rfind("struct Cursor {")));
}

TEST(WgslEmitterTests, RejectsDistinctStructsSharingAName) {
  // Two structurally different types named "S" cannot both be emitted; the emitter fails
  // closed instead of declaring one and silently mistyping the other.
  ModuleBuilder builder;
  const IrType first =
      GetShaderResultOrFail(IrType::Struct("S", {{"a", IrType::F32()}}), IrType::F32());
  const IrType second = GetShaderResultOrFail(
      IrType::Struct("S", {{"a", IrType::U32()}, {"b", IrType::U32()}}), IrType::F32());
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "firstBinding", first), IsShaderOk());
  EXPECT_THAT(builder.addUniformBuffer(0, 1, "secondBinding", second), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitWgsl(module.result()),
              IsShaderError(HasSubstr("two distinct struct types share the name S")));
}

TEST(WgslEmitterTests, RejectsExtendedReservedWords) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addConstant("private", LiteralU32(1)), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitWgsl(module.result()),
              IsShaderError(HasSubstr("collides with a WGSL reserved word")));
}

TEST(WgslEmitterTests, RejectsReservedWordIdentifiers) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addConstant("loop", LiteralU32(1)), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitWgsl(module.result()),
              IsShaderError(HasSubstr("collides with a WGSL reserved word")));
}

TEST(WgslEmitterTests, RejectsPaddedUniformArrays) {
  // array<f32, 4> in uniform needs a padded element wrapper (natural stride 4); the emitter
  // fails closed instead of silently emitting a layout mismatch.
  ModuleBuilder builder;
  const IrType arrayType =
      GetShaderResultOrFail(IrType::SizedArray(IrType::F32(), 4), IrType::F32());
  const IrType uniformType =
      GetShaderResultOrFail(IrType::Struct("BadUniforms", {{"taps", arrayType}}), IrType::F32());
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "badUniforms", uniformType), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitWgsl(module.result()), IsShaderError(HasSubstr("padded element wrapper")));
}

TEST(WgslEmitterTests, RejectsNonFiniteFloatLiterals) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addConstant("kBad", LiteralF32(std::numeric_limits<float>::infinity())),
              IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());
  EXPECT_THAT(EmitWgsl(module.result()), IsShaderError(HasSubstr("non-finite")));
}

}  // namespace
}  // namespace donner::gpu::shader
