/// @file
/// Accept/reject tests for the validating IR builder: types, expressions, statements, module
/// bindings, and stage rules.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu::shader {
namespace {

IrExpr F32Val() {
  return LiteralF32(1.0f);
}
IrExpr Vec2fVal() {
  return GetShaderResultOrFail(
      ConstructVector(IrType::Vec2f(), {LiteralF32(1.0f), LiteralF32(2.0f)}), LiteralF32(0));
}
IrExpr Vec4fVal() {
  return GetShaderResultOrFail(
      ConstructVector(IrType::Vec4f(),
                      {LiteralF32(1.0f), LiteralF32(2.0f), LiteralF32(3.0f), LiteralF32(4.0f)}),
      LiteralF32(0));
}
IrExpr Mat4Val() {
  return GetShaderResultOrFail(ConstructMat4x4f({Vec4fVal(), Vec4fVal(), Vec4fVal(), Vec4fVal()}),
                               LiteralF32(0));
}

// == Types ====================================================================================

TEST(IrTypeTests, IdenticalTypesCompareEqual) {
  EXPECT_TRUE(IrType::Vec2f() == IrType::Vec2(ScalarKind::F32));
  EXPECT_FALSE(IrType::Vec2f() == IrType::Vec2i());
  EXPECT_FALSE(IrType::Vec2f() == IrType::Vec3f());

  const IrType arrayA =
      GetShaderResultOrFail(IrType::SizedArray(IrType::Vec4f(), 4), IrType::F32());
  const IrType arrayB =
      GetShaderResultOrFail(IrType::SizedArray(IrType::Vec4f(), 4), IrType::F32());
  EXPECT_TRUE(arrayA == arrayB);

  const IrType structA =
      GetShaderResultOrFail(IrType::Struct("S", {{"a", IrType::F32()}}), IrType::F32());
  const IrType structB =
      GetShaderResultOrFail(IrType::Struct("S", {{"a", IrType::F32()}}), IrType::F32());
  const IrType structC =
      GetShaderResultOrFail(IrType::Struct("S", {{"a", IrType::U32()}}), IrType::F32());
  EXPECT_TRUE(structA == structB);
  EXPECT_FALSE(structA == structC);
}

TEST(IrTypeTests, RejectsInvalidArraysAndStructs) {
  EXPECT_THAT(IrType::SizedArray(IrType::F32(), 0),
              IsShaderError(HasSubstr("count must be nonzero")));
  EXPECT_THAT(IrType::SizedArray(IrType::Texture2dF32(), 2),
              IsShaderError(HasSubstr("not plain data")));

  const IrType runtimeArray =
      GetShaderResultOrFail(IrType::RuntimeArray(IrType::F32()), IrType::F32());
  EXPECT_THAT(IrType::SizedArray(runtimeArray, 2), IsShaderError(HasSubstr("not plain data")));

  EXPECT_THAT(IrType::Struct("Empty", {}), IsShaderError(HasSubstr("at least one member")));
  EXPECT_THAT(IrType::Struct("Dup", {{"a", IrType::F32()}, {"a", IrType::U32()}}),
              IsShaderError(HasSubstr("duplicate member name a")));
  EXPECT_THAT(IrType::Struct("Bad", {{"tex", IrType::Texture2dF32()}}),
              IsShaderError(HasSubstr("not plain data")));
}

// == Expressions ==============================================================================

TEST(IrExprTests, ArithmeticRequiresMatchingNumericTypes) {
  EXPECT_THAT(Add(F32Val(), F32Val()), HasShaderResult());
  EXPECT_THAT(Add(F32Val(), Vec2fVal()), IsShaderError(HasSubstr("matching numeric")));
  EXPECT_THAT(Add(LiteralBool(true), LiteralBool(false)),
              IsShaderError(HasSubstr("matching numeric")));
  EXPECT_THAT(Div(LiteralU32(4), LiteralU32(2)), HasShaderResult());
}

TEST(IrExprTests, MulSupportsMatrixAndScalarVectorForms) {
  // mat4x4f * vec4f and mat4x4f * mat4x4f (slug_fill composes the MVP with the instance matrix).
  const IrExpr matVec = GetShaderResultOrFail(Mul(Mat4Val(), Vec4fVal()), LiteralF32(0));
  EXPECT_TRUE(matVec.type() == IrType::Vec4f());
  const IrExpr matMat = GetShaderResultOrFail(Mul(Mat4Val(), Mat4Val()), LiteralF32(0));
  EXPECT_TRUE(matMat.type() == IrType::Mat4x4f());
  EXPECT_THAT(Mul(Mat4Val(), Vec2fVal()), IsShaderError(HasSubstr("mat4x4<f32> can multiply")));

  // vector * scalar and scalar * vector.
  const IrExpr vecScalar = GetShaderResultOrFail(Mul(Vec2fVal(), F32Val()), LiteralF32(0));
  EXPECT_TRUE(vecScalar.type() == IrType::Vec2f());
  const IrExpr scalarVec = GetShaderResultOrFail(Mul(F32Val(), Vec2fVal()), LiteralF32(0));
  EXPECT_TRUE(scalarVec.type() == IrType::Vec2f());
}

TEST(IrExprTests, ComparisonsAreScalarOnlyAndYieldBool) {
  const IrExpr cmp = GetShaderResultOrFail(Lt(F32Val(), F32Val()), LiteralF32(0));
  EXPECT_TRUE(cmp.type() == IrType::Bool());
  EXPECT_THAT(Ge(Vec2fVal(), Vec2fVal()), IsShaderError(HasSubstr("scalar")));
  EXPECT_THAT(Eq(LiteralBool(true), LiteralBool(false)), HasShaderResult());
  EXPECT_THAT(Lt(LiteralBool(true), LiteralBool(false)), IsShaderError(HasSubstr("numeric")));
}

TEST(IrExprTests, LogicalOpsRequireBool) {
  EXPECT_THAT(And(LiteralBool(true), LiteralBool(false)), HasShaderResult());
  EXPECT_THAT(Or(LiteralBool(true), F32Val()), IsShaderError(HasSubstr("must be bool")));
  EXPECT_THAT(Not(F32Val()), IsShaderError(HasSubstr("requires bool")));
}

TEST(IrExprTests, NegRejectsU32AndBool) {
  EXPECT_THAT(Neg(F32Val()), HasShaderResult());
  EXPECT_THAT(Neg(LiteralI32(-2)), HasShaderResult());
  EXPECT_THAT(Neg(LiteralU32(1)), IsShaderError(HasSubstr("unary minus")));
  EXPECT_THAT(Neg(LiteralBool(true)), IsShaderError(HasSubstr("unary minus")));
}

TEST(IrExprTests, SwizzleValidatesComponents) {
  const IrExpr xy = GetShaderResultOrFail(Swizzle(Vec4fVal(), "xy"), LiteralF32(0));
  EXPECT_TRUE(xy.type() == IrType::Vec2f());
  const IrExpr x = GetShaderResultOrFail(Swizzle(Vec2fVal(), "x"), LiteralF32(0));
  EXPECT_TRUE(x.type() == IrType::F32());

  EXPECT_THAT(Swizzle(Vec2fVal(), "q"), IsShaderError(HasSubstr("out of range")));
  EXPECT_THAT(Swizzle(Vec2fVal(), "xyz"), IsShaderError(HasSubstr("out of range")));
  EXPECT_THAT(Swizzle(Vec2fVal(), ""), IsShaderError(HasSubstr("1-4 components")));
  EXPECT_THAT(Swizzle(F32Val(), "x"), IsShaderError(HasSubstr("requires a vector")));
}

TEST(IrExprTests, MemberAccessValidatesStructMembers) {
  const IrType structType =
      GetShaderResultOrFail(IrType::Struct("Quad", {{"p0", IrType::Vec2f()}}), IrType::F32());
  const IrExpr structRef = MakeRef(RefKind::Let, "q", structType);

  const IrExpr member = GetShaderResultOrFail(Member(structRef, "p0"), LiteralF32(0));
  EXPECT_TRUE(member.type() == IrType::Vec2f());
  EXPECT_THAT(Member(structRef, "missing"), IsShaderError(HasSubstr("no member named")));
  EXPECT_THAT(Member(F32Val(), "x"), IsShaderError(HasSubstr("requires a struct")));
}

TEST(IrExprTests, IndexValidatesBaseAndIndexTypes) {
  const IrType runtimeArray =
      GetShaderResultOrFail(IrType::RuntimeArray(IrType::F32()), IrType::F32());
  const IrExpr arrayRef = MakeRef(RefKind::Resource, "curveData", runtimeArray);

  const IrExpr element = GetShaderResultOrFail(Index(arrayRef, LiteralU32(3)), LiteralF32(0));
  EXPECT_TRUE(element.type() == IrType::F32());

  const IrExpr vectorElement =
      GetShaderResultOrFail(Index(Vec4fVal(), LiteralI32(1)), LiteralF32(0));
  EXPECT_TRUE(vectorElement.type() == IrType::F32());

  EXPECT_THAT(Index(arrayRef, F32Val()), IsShaderError(HasSubstr("index must be i32 or u32")));
  EXPECT_THAT(Index(F32Val(), LiteralU32(0)), IsShaderError(HasSubstr("not indexable")));
}

TEST(IrExprTests, ConstructorsValidateArityAndTypes) {
  EXPECT_THAT(ConstructVector(IrType::Vec4f(), {Vec2fVal(), LiteralF32(0), LiteralF32(1)}),
              HasShaderResult());
  EXPECT_THAT(ConstructVector(IrType::Vec4f(), {Vec2fVal(), LiteralF32(0)}),
              IsShaderError(HasSubstr("needs 4 components, got 3")));
  EXPECT_THAT(ConstructVector(IrType::Vec2f(), {LiteralI32(0), LiteralI32(1)}),
              IsShaderError(HasSubstr("does not match target")));

  // Single-scalar splat, e.g. vec2i(0).
  const IrExpr splat =
      GetShaderResultOrFail(ConstructVector(IrType::Vec2i(), {LiteralI32(0)}), LiteralF32(0));
  EXPECT_TRUE(splat.type() == IrType::Vec2i());

  EXPECT_THAT(ConstructMat4x4f({Vec4fVal(), Vec4fVal()}),
              IsShaderError(HasSubstr("needs 4 columns")));
  EXPECT_THAT(ConstructMat4x4f({Vec4fVal(), Vec4fVal(), Vec4fVal(), Vec2fVal()}),
              IsShaderError(HasSubstr("columns must be vec4<f32>")));
}

TEST(IrExprTests, ConversionsValidateShapes) {
  EXPECT_THAT(Convert(IrType::F32(), LiteralU32(4)), HasShaderResult());
  EXPECT_THAT(Convert(IrType::I32(), LiteralU32(4)), HasShaderResult());
  EXPECT_THAT(Convert(IrType::Vec2i(), Vec2fVal()), HasShaderResult());
  EXPECT_THAT(Convert(IrType::Vec2i(), LiteralI32(0)), HasShaderResult());  // Splat.
  EXPECT_THAT(Convert(IrType::Vec4f(), Vec2fVal()), IsShaderError(HasSubstr("cannot convert")));
  EXPECT_THAT(Convert(IrType::Bool(), F32Val()),
              IsShaderError(HasSubstr("numeric scalar or vector")));
}

TEST(IrExprTests, BuiltinCallsTypeCheck) {
  EXPECT_THAT(CallBuiltin(BuiltinFn::Clamp, {F32Val(), F32Val(), F32Val()}), HasShaderResult());
  EXPECT_THAT(CallBuiltin(BuiltinFn::Clamp, {F32Val(), F32Val()}),
              IsShaderError(HasSubstr("expects 3 arguments")));
  EXPECT_THAT(CallBuiltin(BuiltinFn::Clamp, {F32Val(), F32Val(), LiteralU32(0)}),
              IsShaderError(HasSubstr("matching numeric")));
  EXPECT_THAT(CallBuiltin(BuiltinFn::Saturate, {LiteralU32(0)}),
              IsShaderError(HasSubstr("f32 scalar or vector")));
  EXPECT_THAT(CallBuiltin(BuiltinFn::Length, {Vec2fVal()}), HasShaderResult());
  EXPECT_THAT(CallBuiltin(BuiltinFn::Length, {F32Val()}), IsShaderError(HasSubstr("float vector")));
  EXPECT_THAT(CallBuiltin(BuiltinFn::Select, {F32Val(), F32Val(), LiteralBool(true)}),
              HasShaderResult());
  EXPECT_THAT(CallBuiltin(BuiltinFn::Select, {F32Val(), F32Val(), F32Val()}),
              IsShaderError(HasSubstr("condition must be bool")));

  const IrExpr texture = MakeRef(RefKind::Resource, "tex", IrType::Texture2dF32());
  const IrExpr sampler = MakeRef(RefKind::Resource, "smp", IrType::SamplerType());
  EXPECT_THAT(CallBuiltin(BuiltinFn::TextureSample, {texture, sampler, Vec2fVal()}),
              HasShaderResult());
  EXPECT_THAT(CallBuiltin(BuiltinFn::TextureSample, {texture, texture, Vec2fVal()}),
              IsShaderError(HasSubstr("textureSample requires")));
  EXPECT_THAT(CallBuiltin(BuiltinFn::TextureDimensions, {texture}), HasShaderResult());

  const IrExpr dims =
      GetShaderResultOrFail(CallBuiltin(BuiltinFn::TextureDimensions, {texture}), LiteralF32(0));
  EXPECT_TRUE(dims.type() == IrType::Vec2u());
}

TEST(IrExprTests, UnknownBuiltinNameIsRejected) {
  EXPECT_THAT(CallBuiltinNamed("clamp", {F32Val(), F32Val(), F32Val()}), HasShaderResult());
  EXPECT_THAT(CallBuiltinNamed("dot", {Vec2fVal(), Vec2fVal()}),
              IsShaderError(HasSubstr("unknown builtin function \"dot\"")));
}

// == Module and function builders =============================================================

TEST(ModuleBuilderTests, RejectsDuplicateBindingsAndNames) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addTexture2d(0, 3, "patternTexture"), IsShaderOk());
  EXPECT_THAT(builder.addSampler(0, 3, "patternSampler"),
              IsShaderError(HasSubstr("(group=0, binding=3) is already in use")));
  EXPECT_THAT(builder.addSampler(0, 4, "patternTexture"),
              IsShaderError(HasSubstr("already exists")));
  EXPECT_THAT(builder.addSampler(0, 4, "patternSampler"), IsShaderOk());
}

TEST(ModuleBuilderTests, ValidatesBindingTypes) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addUniformBuffer(0, 0, "uniforms", IrType::F32()),
              IsShaderError(HasSubstr("must be a struct")));
  EXPECT_THAT(builder.addReadOnlyStorageBuffer(0, 1, "data", IrType::Vec4f()),
              IsShaderError(HasSubstr("runtime array or struct")));

  const IrType runtimeArray =
      GetShaderResultOrFail(IrType::RuntimeArray(IrType::F32()), IrType::F32());
  EXPECT_THAT(builder.addReadOnlyStorageBuffer(0, 1, "data", runtimeArray), IsShaderOk());
}

TEST(ModuleBuilderTests, ConstantsMustBeLiterals) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.addConstant("kNoBand", LiteralU32(0xFFFFFFFFu)), IsShaderOk());
  EXPECT_THAT(builder.addConstant("kBad", Vec2fVal()),
              IsShaderError(HasSubstr("must be literals")));
}

class FunctionBuilderTests : public testing::Test {
protected:
  /// Starts a plain void function with an f32 parameter `t` and a vec2f parameter `p`.
  FunctionBuilder startFunction(const std::optional<IrType>& returnType = std::nullopt) {
    auto result = builder_.createFunction(
        "helper", {IrParam{"t", IrType::F32()}, IrParam{"p", IrType::Vec2f()}}, returnType);
    EXPECT_THAT(result, HasShaderResult());
    return std::move(result).result();
  }

  ModuleBuilder builder_;
};

TEST_F(FunctionBuilderTests, RefResolvesParamsAndRejectsUnknownNames) {
  FunctionBuilder function = startFunction();
  EXPECT_THAT(function.ref("t"), HasShaderResult());
  EXPECT_THAT(function.ref("unknown"), IsShaderError(HasSubstr("unknown name")));
}

TEST_F(FunctionBuilderTests, DuplicateLocalNamesAreRejected) {
  FunctionBuilder function = startFunction();
  EXPECT_THAT(function.addLet("x", LiteralF32(1.0f)), HasShaderResult());
  EXPECT_THAT(function.addLet("x", LiteralF32(2.0f)), IsShaderError(HasSubstr("already declared")));
  // The duplicate declaration poisons the builder; finish returns the first error.
  EXPECT_THAT(function.finish(), IsShaderError(HasSubstr("already declared")));
}

TEST_F(FunctionBuilderTests, AssignRequiresMutableLvalueAndMatchingTypes) {
  FunctionBuilder function = startFunction();
  const IrExpr let = GetShaderResultOrFail(function.addLet("x", LiteralF32(1)), LiteralF32(0));
  const IrExpr var =
      GetShaderResultOrFail(function.addVar("y", IrType::F32(), LiteralF32(0)), LiteralF32(0));

  EXPECT_THAT(function.assign(var, LiteralF32(2)), IsShaderOk());
  EXPECT_THAT(function.assign(let, LiteralF32(2)),
              IsShaderError(HasSubstr("not a mutable lvalue")));
}

TEST_F(FunctionBuilderTests, AssignThroughVarChainsIncludingSwizzledComponent) {
  FunctionBuilder function = startFunction();
  const IrExpr vec =
      GetShaderResultOrFail(function.addVar("v", IrType::Vec4f(), Vec4fVal()), LiteralF32(0));

  const IrExpr component = GetShaderResultOrFail(Swizzle(vec, "x"), LiteralF32(0));
  EXPECT_THAT(function.assign(component, LiteralF32(5)), IsShaderOk());

  // Multi-component swizzles are not assignable.
  const IrExpr twoComponents = GetShaderResultOrFail(Swizzle(vec, "xy"), LiteralF32(0));
  EXPECT_THAT(function.assign(twoComponents, Vec2fVal()),
              IsShaderError(HasSubstr("not a mutable lvalue")));
}

TEST_F(FunctionBuilderTests, VarInitializerMustMatchDeclaredType) {
  FunctionBuilder function = startFunction();
  EXPECT_THAT(function.addVar("v", IrType::F32(), LiteralU32(0)),
              IsShaderError(HasSubstr("does not match declared type")));
}

TEST_F(FunctionBuilderTests, IfConditionMustBeBool) {
  FunctionBuilder function = startFunction();
  EXPECT_THAT(function.beginIf(LiteralF32(1)), IsShaderError(HasSubstr("must be bool")));
}

TEST_F(FunctionBuilderTests, BreakAndContinueRequireALoop) {
  FunctionBuilder function = startFunction();
  EXPECT_THAT(function.breakStmt(), IsShaderError(HasSubstr("outside of a loop")));
}

TEST_F(FunctionBuilderTests, ForLoopWithBreakAndContinue) {
  FunctionBuilder function = startFunction();
  const IrExpr i = GetShaderResultOrFail(function.beginFor("i", LiteralU32(0)), LiteralF32(0));
  EXPECT_THAT(function.forCondition(GetShaderResultOrFail(Lt(i, LiteralU32(4)), LiteralF32(0))),
              IsShaderOk());
  EXPECT_THAT(
      function.forContinuing(i, GetShaderResultOrFail(Add(i, LiteralU32(1)), LiteralF32(0))),
      IsShaderOk());
  EXPECT_THAT(function.continueStmt(), IsShaderOk());
  EXPECT_THAT(function.breakStmt(), IsShaderOk());
  EXPECT_THAT(function.endFor(), IsShaderOk());
  EXPECT_THAT(function.finish(), IsShaderOk());
}

TEST_F(FunctionBuilderTests, FinishWithOpenBlockFails) {
  FunctionBuilder function = startFunction();
  EXPECT_THAT(function.beginIf(LiteralBool(true)), IsShaderOk());
  EXPECT_THAT(function.finish(), IsShaderError(HasSubstr("open if/for block")));
}

TEST_F(FunctionBuilderTests, ReturnTypeIsChecked) {
  FunctionBuilder function = startFunction(IrType::F32());
  EXPECT_THAT(function.returnValue(LiteralU32(0)),
              IsShaderError(HasSubstr("return type mismatch")));
}

TEST_F(FunctionBuilderTests, DiscardIsFragmentOnly) {
  FunctionBuilder function = startFunction();
  EXPECT_THAT(function.discard(), IsShaderError(HasSubstr("only valid in fragment entry points")));
}

TEST(EntryPointTests, VertexValidatesBuiltinsAndPosition) {
  ModuleBuilder builder;

  // Missing position output.
  EXPECT_THAT(builder.createVertexEntryPoint("vsMain", {IrParam{"pos", IrType::Vec2f(), 0}},
                                             {IrOutputMember{"uv", IrType::Vec2f(), 0}}),
              IsShaderError(HasSubstr("exactly one position builtin")));

  // Wrong builtin input type.
  EXPECT_THAT(
      builder.createVertexEntryPoint(
          "vsMain",
          {IrParam{"instance_index", IrType::F32(), std::nullopt, BuiltinInput::InstanceIndex}},
          {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position}}),
      IsShaderError(HasSubstr("instance_index: u32")));

  // Duplicate input locations.
  EXPECT_THAT(
      builder.createVertexEntryPoint(
          "vsMain", {IrParam{"a", IrType::Vec2f(), 0}, IrParam{"b", IrType::Vec2f(), 0}},
          {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position}}),
      IsShaderError(HasSubstr("duplicate stage IO location 0")));
}

TEST(EntryPointTests, FragmentValidatesColorOutputAndDiscard) {
  ModuleBuilder builder;

  EXPECT_THAT(builder.createFragmentEntryPoint("fsMain", {IrParam{"uv", IrType::Vec2f(), 0}},
                                               {IrOutputMember{"color", IrType::Vec2f(), 0}}),
              IsShaderError(HasSubstr("location-0 vec4<f32> color output")));

  auto fragment = builder.createFragmentEntryPoint("fsMain", {IrParam{"uv", IrType::Vec2f(), 0}},
                                                   {IrOutputMember{"color", IrType::Vec4f(), 0}});
  ASSERT_THAT(fragment, HasShaderResult());
  FunctionBuilder function = std::move(fragment).result();
  EXPECT_THAT(function.discard(), IsShaderOk());
  EXPECT_THAT(function.returnOutputs({Vec4fVal()}), IsShaderOk());
  EXPECT_THAT(function.finish(), IsShaderOk());
}

TEST(EntryPointTests, FragmentAcceptsPositionBuiltinInput) {
  // slug_fill's fs_main consumes @builtin(position) as pixel_center; the IR must express it.
  ModuleBuilder builder;
  auto fragment = builder.createFragmentEntryPoint(
      "fsMain",
      {IrParam{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinInput::Position},
       IrParam{"sample_pos", IrType::Vec2f(), 0}},
      {IrOutputMember{"color", IrType::Vec4f(), 0}});
  ASSERT_THAT(fragment, HasShaderResult());
  FunctionBuilder function = std::move(fragment).result();

  // The builtin param resolves like any other input.
  const IrExpr clipPos = GetShaderResultOrFail(function.ref("clip_pos"), LiteralF32(0));
  EXPECT_EQ(clipPos.type(), IrType::Vec4f());
  const IrExpr pixelCenter = GetShaderResultOrFail(Swizzle(clipPos, "xy"), LiteralF32(0));
  EXPECT_EQ(pixelCenter.type(), IrType::Vec2f());

  EXPECT_THAT(function.returnOutputs({Vec4fVal()}), IsShaderOk());
  EXPECT_THAT(function.finish(), IsShaderOk());
}

TEST(EntryPointTests, FragmentPositionBuiltinMustBeVec4f) {
  ModuleBuilder builder;
  EXPECT_THAT(
      builder.createFragmentEntryPoint(
          "fsMain", {IrParam{"clip_pos", IrType::Vec2f(), std::nullopt, BuiltinInput::Position}},
          {IrOutputMember{"color", IrType::Vec4f(), 0}}),
      IsShaderError(HasSubstr("position: vec4<f32>")));
}

TEST(EntryPointTests, FragmentRejectsInstanceIndexBuiltin) {
  ModuleBuilder builder;
  EXPECT_THAT(builder.createFragmentEntryPoint("fsMain",
                                               {IrParam{"instance_index", IrType::U32(),
                                                        std::nullopt, BuiltinInput::InstanceIndex}},
                                               {IrOutputMember{"color", IrType::Vec4f(), 0}}),
              IsShaderError(HasSubstr("position")));
}

TEST(EntryPointTests, VertexRejectsPositionBuiltinInput) {
  ModuleBuilder builder;
  EXPECT_THAT(
      builder.createVertexEntryPoint(
          "vsMain", {IrParam{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinInput::Position}},
          {IrOutputMember{"out_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position}}),
      IsShaderError(HasSubstr("instance_index")));
}

TEST(EntryPointTests, ReturnOutputsTypeChecked) {
  ModuleBuilder builder;
  auto vertex = builder.createVertexEntryPoint(
      "vsMain", {IrParam{"pos", IrType::Vec2f(), 0}},
      {IrOutputMember{"clip_pos", IrType::Vec4f(), std::nullopt, BuiltinOutput::Position},
       IrOutputMember{"sample_pos", IrType::Vec2f(), 0}});
  ASSERT_THAT(vertex, HasShaderResult());
  FunctionBuilder function = std::move(vertex).result();

  EXPECT_THAT(function.returnOutputs({Vec4fVal()}),
              IsShaderError(HasSubstr("declares 2 outputs, got 1")));
}

TEST(ModuleBuilderTests, CallFunctionChecksSignature) {
  ModuleBuilder builder;
  {
    auto helper = builder.createFunction("scale", {IrParam{"x", IrType::F32()}}, IrType::F32());
    ASSERT_THAT(helper, HasShaderResult());
    FunctionBuilder function = std::move(helper).result();
    const IrExpr x = GetShaderResultOrFail(function.ref("x"), LiteralF32(0));
    EXPECT_THAT(function.returnValue(GetShaderResultOrFail(Mul(x, LiteralF32(2)), LiteralF32(0))),
                IsShaderOk());
    EXPECT_THAT(function.finish(), IsShaderOk());
  }

  // Errors poison the function builder, so each rejection uses a fresh (abandoned) builder.
  {
    auto caller = builder.createFunction("callerA", {}, IrType::F32());
    ASSERT_THAT(caller, HasShaderResult());
    FunctionBuilder function = std::move(caller).result();
    EXPECT_THAT(function.callFunction("missing", {LiteralF32(1)}),
                IsShaderError(HasSubstr("unknown function")));
  }
  {
    auto caller = builder.createFunction("callerB", {}, IrType::F32());
    ASSERT_THAT(caller, HasShaderResult());
    FunctionBuilder function = std::move(caller).result();
    EXPECT_THAT(function.callFunction("scale", {}),
                IsShaderError(HasSubstr("takes 1 arguments, got 0")));
    EXPECT_THAT(function.callFunction("scale", {LiteralU32(1)}),
                IsShaderError(HasSubstr("takes 1 arguments, got 0")));  // Poisoned: first error.
  }
}

TEST(ModuleBuilderTests, ResourceRefsResolveInsideFunctions) {
  ModuleBuilder builder;
  const IrType bandArray = GetShaderResultOrFail(
      IrType::RuntimeArray(GetShaderResultOrFail(
          IrType::Struct("Band", {{"curveStart", IrType::U32()}}), IrType::F32())),
      IrType::F32());
  EXPECT_THAT(builder.addReadOnlyStorageBuffer(0, 1, "bands", bandArray), IsShaderOk());
  EXPECT_THAT(builder.addConstant("kNoBand", LiteralU32(0xFFFFFFFFu)), IsShaderOk());

  auto helper = builder.createFunction("probe", {}, IrType::U32());
  ASSERT_THAT(helper, HasShaderResult());
  FunctionBuilder function = std::move(helper).result();

  const IrExpr bands = GetShaderResultOrFail(function.ref("bands"), LiteralF32(0));
  const IrExpr band = GetShaderResultOrFail(Index(bands, LiteralU32(0)), LiteralF32(0));
  const IrExpr curveStart = GetShaderResultOrFail(Member(band, "curveStart"), LiteralF32(0));
  EXPECT_THAT(function.ref("kNoBand"), HasShaderResult());
  EXPECT_THAT(function.returnValue(curveStart), IsShaderOk());
  EXPECT_THAT(function.finish(), IsShaderOk());
}

}  // namespace
}  // namespace donner::gpu::shader
