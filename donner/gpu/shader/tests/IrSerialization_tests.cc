/// @file
/// Deterministic serialization tests: byte-identical repeats and an embedded golden of a
/// representative module exercising every node kind.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;
using testing::Not;

namespace donner::gpu::shader {
namespace {

/// Builds a representative module exercising every node kind: a constant, all four binding
/// kinds, a plain function (for/if/else/break/continue/index/member/swizzle/convert/builtins),
/// and vertex + fragment entry points (construct, mat*vec, select, textureSample, discard).
IrModule BuildRepresentativeModule() {
  ModuleBuilder builder;

  const IrType uniformsType = GetShaderResultOrFail(
      IrType::Struct(
          "Params",
          {{"mvp", IrType::Mat4x4f()}, {"color", IrType::Vec4f()}, {"fillRule", IrType::U32()}}),
      IrType::F32());
  const IrType bandType = GetShaderResultOrFail(
      IrType::Struct("Band", {{"curveStart", IrType::U32()}, {"curveCount", IrType::U32()}}),
      IrType::F32());
  const IrType bandArray = GetShaderResultOrFail(IrType::RuntimeArray(bandType), IrType::F32());

  EXPECT_THAT(builder.addConstant("kNoBand", LiteralU32(0xFFFFFFFFu)), IsShaderOk());
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "params", uniformsType), IsShaderOk());
  EXPECT_THAT(builder.addReadOnlyStorageBuffer(0, 1, "bands", bandArray), IsShaderOk());
  EXPECT_THAT(builder.addTexture2d(0, 2, "patternTexture"), IsShaderOk());
  EXPECT_THAT(builder.addSampler(0, 3, "patternSampler"), IsShaderOk());

  // Plain function: sums curveCounts of the first `limit` bands, skipping empty entries.
  {
    auto result =
        builder.createFunction("countCurves", {IrParam{"limit", IrType::U32()}}, IrType::U32());
    EXPECT_THAT(result, HasShaderResult());
    FunctionBuilder function = std::move(result).result();

    const IrExpr limit = GetShaderResultOrFail(function.ref("limit"), LiteralF32(0));
    const IrExpr total = GetShaderResultOrFail(
        function.addVar("total", IrType::U32(), LiteralU32(0)), LiteralF32(0));
    const IrExpr bands = GetShaderResultOrFail(function.ref("bands"), LiteralF32(0));
    const IrExpr sentinel = GetShaderResultOrFail(function.ref("kNoBand"), LiteralF32(0));

    const IrExpr i = GetShaderResultOrFail(function.beginFor("i", LiteralU32(0)), LiteralF32(0));
    EXPECT_THAT(function.forCondition(GetShaderResultOrFail(Lt(i, limit), LiteralF32(0))),
                IsShaderOk());
    EXPECT_THAT(
        function.forContinuing(i, GetShaderResultOrFail(Add(i, LiteralU32(1)), LiteralF32(0))),
        IsShaderOk());

    const IrExpr band = GetShaderResultOrFail(Index(bands, i), LiteralF32(0));
    const IrExpr curveStart = GetShaderResultOrFail(Member(band, "curveStart"), LiteralF32(0));
    EXPECT_THAT(function.beginIf(GetShaderResultOrFail(Eq(curveStart, sentinel), LiteralF32(0))),
                IsShaderOk());
    EXPECT_THAT(function.continueStmt(), IsShaderOk());
    EXPECT_THAT(function.elseBranch(), IsShaderOk());
    EXPECT_THAT(
        function.beginIf(GetShaderResultOrFail(Gt(curveStart, LiteralU32(1000)), LiteralF32(0))),
        IsShaderOk());
    EXPECT_THAT(function.breakStmt(), IsShaderOk());
    EXPECT_THAT(function.endIf(), IsShaderOk());
    EXPECT_THAT(function.endIf(), IsShaderOk());

    const IrExpr curveCount = GetShaderResultOrFail(Member(band, "curveCount"), LiteralF32(0));
    EXPECT_THAT(
        function.assign(total, GetShaderResultOrFail(Add(total, curveCount), LiteralF32(0))),
        IsShaderOk());
    EXPECT_THAT(function.endFor(), IsShaderOk());
    EXPECT_THAT(function.returnValue(total), IsShaderOk());
    EXPECT_THAT(function.finish(), IsShaderOk());
  }

  // Vertex entry point: instance_index builtin, mat*vec multiply, constructors, swizzle.
  {
    auto result = builder.createVertexEntryPoint(
        "vsMain",
        {IrParam{"instance_index", IrType::U32(), std::nullopt, BuiltinInput::InstanceIndex},
         IrParam{"pos", IrType::Vec2f(), 0}},
        {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position},
         IrOutputMember{"sample_pos", IrType::Vec2f(), 0}});
    EXPECT_THAT(result, HasShaderResult());
    FunctionBuilder function = std::move(result).result();

    const IrExpr pos = GetShaderResultOrFail(function.ref("pos"), LiteralF32(0));
    const IrExpr params = GetShaderResultOrFail(function.ref("params"), LiteralF32(0));
    const IrExpr mvp = GetShaderResultOrFail(Member(params, "mvp"), LiteralF32(0));
    const IrExpr homogeneous = GetShaderResultOrFail(
        ConstructVector(IrType::Vec4f(), {pos, LiteralF32(0), LiteralF32(1)}), LiteralF32(0));
    const IrExpr clipPos = GetShaderResultOrFail(
        function.addLet("clip", GetShaderResultOrFail(Mul(mvp, homogeneous), LiteralF32(0))),
        LiteralF32(0));
    const IrExpr samplePos = GetShaderResultOrFail(Swizzle(clipPos, "xy"), LiteralF32(0));
    EXPECT_THAT(function.returnOutputs({clipPos, samplePos}), IsShaderOk());
    EXPECT_THAT(function.finish(), IsShaderOk());
  }

  // Fragment entry point: user call, builtins, select, textureSample/Load/Dimensions, negation,
  // logical ops, conversion, single-component swizzle assignment, discard.
  {
    auto result =
        builder.createFragmentEntryPoint("fsMain", {IrParam{"sample_pos", IrType::Vec2f(), 0}},
                                         {IrOutputMember{"color", IrType::Vec4f(), 0}});
    EXPECT_THAT(result, HasShaderResult());
    FunctionBuilder function = std::move(result).result();

    const IrExpr samplePos = GetShaderResultOrFail(function.ref("sample_pos"), LiteralF32(0));
    const IrExpr params = GetShaderResultOrFail(function.ref("params"), LiteralF32(0));
    const IrExpr texture = GetShaderResultOrFail(function.ref("patternTexture"), LiteralF32(0));
    const IrExpr sampler = GetShaderResultOrFail(function.ref("patternSampler"), LiteralF32(0));

    const IrExpr count = GetShaderResultOrFail(
        function.addLet("count",
                        GetShaderResultOrFail(function.callFunction("countCurves", {LiteralU32(4)}),
                                              LiteralF32(0))),
        LiteralF32(0));

    const IrExpr coverage = GetShaderResultOrFail(
        function.addLet(
            "coverage",
            GetShaderResultOrFail(
                CallBuiltin(
                    BuiltinFn::Saturate,
                    {GetShaderResultOrFail(
                        CallBuiltin(BuiltinFn::Fract, {GetShaderResultOrFail(
                                                          Swizzle(samplePos, "x"), LiteralF32(0))}),
                        LiteralF32(0))}),
                LiteralF32(0))),
        LiteralF32(0));

    // if (coverage <= 0.0 && count > 0u) { discard; }
    const IrExpr coverageZero = GetShaderResultOrFail(Le(coverage, LiteralF32(0)), LiteralF32(0));
    const IrExpr hasCurves = GetShaderResultOrFail(Gt(count, LiteralU32(0)), LiteralF32(0));
    EXPECT_THAT(
        function.beginIf(GetShaderResultOrFail(And(coverageZero, hasCurves), LiteralF32(0))),
        IsShaderOk());
    EXPECT_THAT(function.discard(), IsShaderOk());
    EXPECT_THAT(function.endIf(), IsShaderOk());

    // Texture path: dims = vec2i(textureDimensions(t)); texel = textureLoad(t, dims - vec2i(1), 0)
    const IrExpr dims = GetShaderResultOrFail(
        function.addLet(
            "dims",
            GetShaderResultOrFail(
                Convert(IrType::Vec2i(),
                        GetShaderResultOrFail(CallBuiltin(BuiltinFn::TextureDimensions, {texture}),
                                              LiteralF32(0))),
                LiteralF32(0))),
        LiteralF32(0));
    const IrExpr texel = GetShaderResultOrFail(
        function.addLet(
            "texel",
            GetShaderResultOrFail(
                CallBuiltin(BuiltinFn::TextureLoad,
                            {texture,
                             GetShaderResultOrFail(
                                 Sub(dims, GetShaderResultOrFail(
                                               ConstructVector(IrType::Vec2i(), {LiteralI32(1)}),
                                               LiteralF32(0))),
                                 LiteralF32(0)),
                             LiteralI32(0)}),
                LiteralF32(0))),
        LiteralF32(0));

    const IrExpr sampled = GetShaderResultOrFail(
        function.addLet("sampled", GetShaderResultOrFail(CallBuiltin(BuiltinFn::TextureSample,
                                                                     {texture, sampler, samplePos}),
                                                         LiteralF32(0))),
        LiteralF32(0));

    // var out = select(params.color, sampled, coverage > 0.5) * coverage; out.w = -(-coverage).
    const IrExpr paramsColor = GetShaderResultOrFail(Member(params, "color"), LiteralF32(0));
    const IrExpr selected = GetShaderResultOrFail(
        CallBuiltin(BuiltinFn::Select,
                    {paramsColor, sampled,
                     GetShaderResultOrFail(Gt(coverage, LiteralF32(0.5f)), LiteralF32(0))}),
        LiteralF32(0));
    const IrExpr out = GetShaderResultOrFail(
        function.addVar("out", IrType::Vec4f(),
                        GetShaderResultOrFail(Mul(selected, coverage), LiteralF32(0))),
        LiteralF32(0));
    const IrExpr outAlpha = GetShaderResultOrFail(Swizzle(out, "w"), LiteralF32(0));
    EXPECT_THAT(
        function.assign(
            outAlpha, GetShaderResultOrFail(
                          Neg(GetShaderResultOrFail(Neg(coverage), LiteralF32(0))), LiteralF32(0))),
        IsShaderOk());
    // Fold in the loaded texel so textureLoad participates in the result.
    EXPECT_THAT(function.assign(out, GetShaderResultOrFail(
                                         CallBuiltin(BuiltinFn::Max, {out, texel}), LiteralF32(0))),
                IsShaderOk());

    EXPECT_THAT(function.returnOutputs({out}), IsShaderOk());
    EXPECT_THAT(function.finish(), IsShaderOk());
  }

  ShaderResult<IrModule> module = builder.build();
  EXPECT_THAT(module, HasShaderResult());
  if (module.hasError()) {
    ModuleBuilder emptyBuilder;
    return std::move(emptyBuilder.build()).result();
  }
  return std::move(module).result();
}

TEST(IrSerializationTests, IdenticalModulesSerializeByteIdentically) {
  const IrModule first = BuildRepresentativeModule();
  const IrModule second = BuildRepresentativeModule();
  EXPECT_THAT(first.serialize(), testing::Eq(second.serialize()));
}

TEST(IrSerializationTests, TypesConstructedInDifferentOrderCompareEqual) {
  // Semantically identical types built in different orders are value-equal, so modules built
  // from them serialize identically.
  const IrType vecFirst = IrType::Vec4f();
  const IrType arrayA = GetShaderResultOrFail(IrType::SizedArray(vecFirst, 4), IrType::F32());
  const IrType arrayB =
      GetShaderResultOrFail(IrType::SizedArray(IrType::Vec4(ScalarKind::F32), 4), IrType::F32());
  EXPECT_EQ(arrayA, arrayB);
  EXPECT_THAT(arrayA.toString(), testing::Eq(arrayB.toString()));
}

TEST(IrSerializationTests, NestedStructDefinitionsAreSerializedRecursively) {
  ModuleBuilder builder;
  const IrType inner = GetShaderResultOrFail(
      IrType::Struct("GridParams", {{"base", IrType::F32()}, {"count", IrType::U32()}}),
      IrType::F32());
  const IrType outer = GetShaderResultOrFail(
      IrType::Struct("NestedUniforms", {{"mvp", IrType::Mat4x4f()}, {"grid", inner}}),
      IrType::F32());
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "params", outer), IsShaderOk());

  ShaderResult<IrModule> module = builder.build();
  ASSERT_THAT(module, HasShaderResult());

  const std::string serialized = module.result().serialize();
  EXPECT_THAT(serialized,
              HasSubstr("struct NestedUniforms { mvp: mat4x4<f32>, grid: GridParams }"));
  EXPECT_THAT(serialized, HasSubstr("struct GridParams { base: f32, count: u32 }"));
}

TEST(IrSerializationTests, SerializationContainsNoPointers) {
  const IrModule module = BuildRepresentativeModule();
  EXPECT_THAT(module.serialize(), testing::Not(HasSubstr("0x")));
}

TEST(IrSerializationTests, GoldenSerializationOfRepresentativeModule) {
  const IrModule module = BuildRepresentativeModule();

  // clang-format off
  const std::string kExpected = R"(module
constant kNoBand: u32 = lit_u32(4294967295)
binding group=0 binding=0 kind=uniform params: Params
  struct Params { mvp: mat4x4<f32>, color: vec4<f32>, fillRule: u32 }
binding group=0 binding=1 kind=storage_read bands: array<Band>
  struct Band { curveStart: u32, curveCount: u32 }
binding group=0 binding=2 kind=texture_2d_f32 patternTexture: texture_2d<f32>
binding group=0 binding=3 kind=sampler patternSampler: sampler
function countCurves
  param limit: u32
  returns u32
  body:
    var total: u32 = lit_u32(0)
    for
      init:
        var i: u32 = lit_u32(0)
      cond: lt(ref(i), ref(limit))
      continuing:
        assign ref(i) = add(ref(i), lit_u32(1))
      body:
        if eq(member(index(ref(bands), ref(i)), curveStart), ref(kNoBand))
          continue
        else
          if gt(member(index(ref(bands), ref(i)), curveStart), lit_u32(1000))
            break
        assign ref(total) = add(ref(total), member(index(ref(bands), ref(i)), curveCount))
    return(ref(total))
function vsMain stage=vertex
  param instance_index: u32 @builtin(instance_index)
  param pos: vec2<f32> @location(0)
  output clip_pos: vec4<f32> @builtin(position)
  output sample_pos: vec2<f32> @location(0)
  body:
    let clip = mul(member(ref(params), mvp), construct_vec4<f32>(ref(pos), lit_f32(0), lit_f32(1)))
    return(ref(clip), swizzle(ref(clip), xy))
function fsMain stage=fragment
  param sample_pos: vec2<f32> @location(0)
  output color: vec4<f32> @location(0)
  body:
    let count = call(countCurves, lit_u32(4))
    let coverage = builtin_saturate(builtin_fract(swizzle(ref(sample_pos), x)))
    if and(le(ref(coverage), lit_f32(0)), gt(ref(count), lit_u32(0)))
      discard
    let dims = convert_vec2<i32>(builtin_textureDimensions(ref(patternTexture)))
    let texel = builtin_textureLoad(ref(patternTexture), sub(ref(dims), construct_vec2<i32>(lit_i32(1))), lit_i32(0))
    let sampled = builtin_textureSample(ref(patternTexture), ref(patternSampler), ref(sample_pos))
    var out: vec4<f32> = mul(builtin_select(member(ref(params), color), ref(sampled), gt(ref(coverage), lit_f32(0.5))), ref(coverage))
    assign swizzle(ref(out), w) = neg(neg(ref(coverage)))
    assign ref(out) = builtin_max(ref(out), ref(texel))
    return(ref(out))
)";
  // clang-format on

  EXPECT_THAT(module.serialize(), testing::Eq(kExpected));
}

}  // namespace
}  // namespace donner::gpu::shader
