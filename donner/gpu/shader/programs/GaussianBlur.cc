#include "donner/gpu/shader/programs/GaussianBlur.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
namespace donner::gpu::shader::programs {
namespace {
IrExpr Four(ErrorLatch& e, float value) {
  return e(ConstructVector(IrType::Vec4f(), {LiteralF32(value), LiteralF32(value),
                                             LiteralF32(value), LiteralF32(value)}));
}
IrExpr Two(ErrorLatch& e, int32_t value) {
  return e(ConstructVector(IrType::Vec2i(), {LiteralI32(value), LiteralI32(value)}));
}
IrExpr Outside(ErrorLatch& e, const IrExpr& coord, const IrExpr& low, const IrExpr& high) {
  return e(Or(e(CallBuiltin(BuiltinFn::Any, {e(Lt(coord, low))})),
              e(CallBuiltin(BuiltinFn::Any, {e(Ge(coord, high))}))));
}
ShaderStatus AddClip(ModuleBuilder& builder) {
  ErrorLatch e;
  auto result = builder.createFunction(
      "applyClip", {IrParam{"coord", IrType::Vec2i()}, IrParam{"value", IrType::Vec4f()}},
      IrType::Vec4f());
  if (result.hasError()) {
    return std::move(result).error();
  }
  auto fn = std::move(result).result();
  const auto params = e(fn.ref("params"));
  e.ok(fn.beginIf(e(Eq(e(Member(params, "clipActive")), LiteralU32(1)))));
  e.ok(fn.beginIf(
      Outside(e, e(fn.ref("coord")), e(Member(params, "clipMin")), e(Member(params, "clipMax")))));
  e.ok(fn.returnValue(Four(e, 0)));
  e.ok(fn.endIf());
  e.ok(fn.endIf());
  e.ok(fn.returnValue(
      e(CallBuiltin(BuiltinFn::Clamp, {e(fn.ref("value")), Four(e, 0), Four(e, 1)}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}
ShaderStatus AddSampleEdge(ModuleBuilder& builder) {
  ErrorLatch e;
  auto result = builder.createFunction(
      "sampleEdge", {IrParam{"coord", IrType::Vec2i()}, IrParam{"size", IrType::Vec2i()}},
      IrType::Vec4f());
  if (result.hasError()) {
    return std::move(result).error();
  }
  auto fn = std::move(result).result();
  const auto coord = e(fn.ref("coord")), size = e(fn.ref("size"));
  const auto input = e(fn.ref("inputTexture"));
  const auto mode = e(Member(e(fn.ref("params")), "edgeMode"));
  e.ok(fn.beginIf(e(Eq(mode, LiteralU32(0)))));
  e.ok(fn.beginIf(Outside(e, coord, Two(e, 0), size)));
  e.ok(fn.returnValue(Four(e, 0)));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::TextureLoad, {input, coord, LiteralI32(0)}))));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Eq(mode, LiteralU32(1)))));
  const auto clamped = e(fn.addLet(
      "clamped", e(CallBuiltin(BuiltinFn::Clamp, {coord, Two(e, 0), e(Sub(size, Two(e, 1)))}))));
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::TextureLoad, {input, clamped, LiteralI32(0)}))));
  e.ok(fn.endIf());
  const auto wrap = [&](const char* axis) {
    const auto length = e(Swizzle(size, axis));
    return e(Mod(e(Add(e(Mod(e(Swizzle(coord, axis)), length)), length)), length));
  };
  const auto wrapped =
      e(fn.addLet("wrapped", e(ConstructVector(IrType::Vec2i(), {wrap("x"), wrap("y")}))));
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::TextureLoad, {input, wrapped, LiteralI32(0)}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}
IrExpr AxisOffset(ErrorLatch& e, const IrExpr& i, const IrExpr& params) {
  return e(CallBuiltin(BuiltinFn::Select, {e(ConstructVector(IrType::Vec2i(), {LiteralI32(0), i})),
                                           e(ConstructVector(IrType::Vec2i(), {i, LiteralI32(0)})),
                                           e(Eq(e(Member(params, "axis")), LiteralU32(0)))}));
}
void StoreClipped(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& output, const IrExpr& coord,
                  const IrExpr& value) {
  e.ok(fn.textureStore(output, coord, e(fn.callFunction("applyClip", {coord, value}))));
}
void AddBoxPass(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& params, const IrExpr& coord,
                const IrExpr& size, const IrExpr& output) {
  e.ok(fn.beginIf(e(Eq(e(Member(params, "kernelType")), LiteralU32(1)))));
  const auto sum = e(fn.addVar("boxSum", IrType::Vec4f(), Four(e, 0)));
  const auto left = e(Member(params, "boxLeft")), right = e(Member(params, "boxRight"));
  const auto i = e(fn.beginFor("boxIndex", e(Neg(left))));
  e.ok(fn.forCondition(e(Le(i, right))));
  e.ok(fn.forContinuing(i, e(Add(i, LiteralI32(1)))));
  const auto sample =
      e(fn.callFunction("sampleEdge", {e(Add(coord, AxisOffset(e, i, params))), size}));
  e.ok(fn.assign(sum, e(Add(sum, sample))));
  e.ok(fn.endFor());
  const auto count = e(Convert(IrType::F32(), e(Add(e(Add(left, right)), LiteralI32(1)))));
  StoreClipped(e, fn, output, coord, e(Div(sum, count)));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
}
void AddGaussianPass(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& params, const IrExpr& coord,
                     const IrExpr& size, const IrExpr& output) {
  const auto sigma = e(Member(params, "stdDeviation"));
  e.ok(fn.beginIf(e(Le(sigma, LiteralF32(0)))));
  StoreClipped(
      e, fn, output, coord,
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("inputTexture")), coord, LiteralI32(0)})));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  const auto radius = e(fn.addLet(
      "radius",
      e(CallBuiltin(BuiltinFn::Min,
                    {e(Convert(IrType::I32(),
                               e(CallBuiltin(BuiltinFn::Ceil, {e(Mul(LiteralF32(3), sigma))})))),
                     LiteralI32(127)}))));
  const auto inverse = e(fn.addLet(
      "inverseVariance", e(Div(LiteralF32(1), e(Mul(e(Mul(LiteralF32(2), sigma)), sigma))))));
  const auto sum = e(fn.addVar("sum", IrType::Vec4f(), Four(e, 0)));
  const auto weightSum = e(fn.addVar("weightSum", IrType::F32(), LiteralF32(0)));
  const auto i = e(fn.beginFor("index", e(Neg(radius))));
  e.ok(fn.forCondition(e(Le(i, radius))));
  e.ok(fn.forContinuing(i, e(Add(i, LiteralI32(1)))));
  const auto distance = e(fn.addLet("distance", e(Convert(IrType::F32(), i))));
  const auto weight = e(fn.addLet(
      "weight",
      e(CallBuiltin(BuiltinFn::Exp, {e(Mul(e(Mul(e(Neg(distance)), distance)), inverse))}))));
  const auto sample =
      e(fn.callFunction("sampleEdge", {e(Add(coord, AxisOffset(e, i, params))), size}));
  e.ok(fn.assign(sum, e(Add(sum, e(Mul(sample, weight))))));
  e.ok(fn.assign(weightSum, e(Add(weightSum, weight))));
  e.ok(fn.endFor());
  e.ok(fn.beginIf(e(Gt(weightSum, LiteralF32(0)))));
  e.ok(fn.assign(sum, e(Div(sum, weightSum))));
  e.ok(fn.endIf());
  StoreClipped(e, fn, output, coord, sum);
}
}  // namespace
ShaderResult<IrModule> BuildGaussianBlurModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  const auto paramsType = e(IrType::Struct(
      "BlurParams",
      {IrType::Member{"stdDeviation", IrType::F32()}, IrType::Member{"axis", IrType::U32()},
       IrType::Member{"edgeMode", IrType::U32()}, IrType::Member{"kernelType", IrType::U32()},
       IrType::Member{"boxLeft", IrType::I32()}, IrType::Member{"boxRight", IrType::I32()},
       IrType::Member{"clipMin", IrType::Vec2i()}, IrType::Member{"clipMax", IrType::Vec2i()},
       IrType::Member{"clipActive", IrType::U32()}, IrType::Member{"pad", IrType::U32()}}));
  e.ok(builder.addTexture2d(0, 0, "inputTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, 1, "outputTexture",
                                            StorageTextureFormat::Rgba32Float));
  e.ok(builder.addUniformBuffer(0, 2, "params", paramsType));
  e.ok(AddClip(builder));
  e.ok(AddSampleEdge(builder));
  auto entry = builder.createComputeEntryPoint(
      RcString(kGaussianBlurEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kGaussianBlurWorkgroupSize, kGaussianBlurWorkgroupSize, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  auto fn = std::move(entry).result();
  const auto output = e(fn.ref("outputTexture")), input = e(fn.ref("inputTexture"));
  const auto params = e(fn.ref("params")), gid = e(fn.ref("gid"));
  const auto extent =
      e(fn.addLet("extent", e(CallBuiltin(BuiltinFn::TextureDimensions, {output}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(extent, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(extent, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  const auto coord = e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const auto size = e(fn.addLet(
      "size", e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions, {input}))))));
  AddBoxPass(e, fn, params, coord, size, output);
  AddGaussianPass(e, fn, params, coord, size, output);
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}
}  // namespace donner::gpu::shader::programs
