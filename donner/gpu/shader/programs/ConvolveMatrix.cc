#include "donner/gpu/shader/programs/ConvolveMatrix.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {
namespace {

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(ConvolveMatrixBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Constructs a vector whose components all equal \p value.
IrExpr Splat(ErrorLatch& e, const IrType& type, float value) {
  return e(ConstructVector(type, {LiteralF32(value)}));
}

/// True when a signed coordinate falls outside `[0, size)` on either axis.
IrExpr IsOutside(ErrorLatch& e, const IrExpr& coord, const IrExpr& size) {
  const IrExpr zero = e(ConstructVector(IrType::Vec2i(), {LiteralI32(0)}));
  return e(Or(e(CallBuiltin(BuiltinFn::Any, {e(Lt(coord, zero))})),
              e(CallBuiltin(BuiltinFn::Any, {e(Ge(coord, size))}))));
}

/// Adds edge-mode sampling through the module's input texture and parameter block.
ShaderStatus AddSampleEdge(ModuleBuilder& builder) {
  ErrorLatch e;
  auto result = builder.createFunction(
      "sampleEdge", {IrParam{"coord", IrType::Vec2i()}, IrParam{"size", IrType::Vec2i()}},
      IrType::Vec4f());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr coord = e(fn.ref("coord"));
  const IrExpr size = e(fn.ref("size"));
  const IrExpr input = e(fn.ref("inputTexture"));
  const IrExpr params = e(fn.ref("params"));
  const IrExpr edgeMode = e(Member(params, "edgeMode"));
  const IrExpr zero = e(ConstructVector(IrType::Vec2i(), {LiteralI32(0)}));
  const IrExpr one = e(ConstructVector(IrType::Vec2i(), {LiteralI32(1)}));

  e.ok(fn.beginIf(e(Eq(edgeMode, LiteralU32(0)))));
  const IrExpr clamped =
      e(fn.addLet("clamped", e(CallBuiltin(BuiltinFn::Clamp, {coord, zero, e(Sub(size, one))}))));
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::TextureLoad, {input, clamped, LiteralI32(0)}))));
  e.ok(fn.endIf());

  e.ok(fn.beginIf(e(Eq(edgeMode, LiteralU32(1)))));
  const auto wrapAxis = [&](const char* axis) {
    const IrExpr coordinate = e(Swizzle(coord, axis));
    const IrExpr extent = e(Swizzle(size, axis));
    return e(Mod(e(Add(e(Mod(coordinate, extent)), extent)), extent));
  };
  const IrExpr wrapped =
      e(fn.addLet("wrapped", e(ConstructVector(IrType::Vec2i(), {wrapAxis("x"), wrapAxis("y")}))));
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::TextureLoad, {input, wrapped, LiteralI32(0)}))));
  e.ok(fn.endIf());

  e.ok(fn.beginIf(IsOutside(e, coord, size)));
  e.ok(fn.returnValue(Splat(e, IrType::Vec4f(), 0.0f)));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::TextureLoad, {input, coord, LiteralI32(0)}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

}  // namespace

ShaderResult<IrModule> BuildConvolveMatrixModule() {
  ErrorLatch e;
  ModuleBuilder builder;

  const IrType kernelType = e(IrType::SizedArray(IrType::F32(), kConvolveMatrixKernelCapacity));
  const IrType paramsType = e(IrType::Struct(
      "ConvolveMatrixParams",
      {IrType::Member{"orderX", IrType::I32()}, IrType::Member{"orderY", IrType::I32()},
       IrType::Member{"targetX", IrType::I32()}, IrType::Member{"targetY", IrType::I32()},
       IrType::Member{"divisor", IrType::F32()}, IrType::Member{"bias", IrType::F32()},
       IrType::Member{"edgeMode", IrType::U32()}, IrType::Member{"preserveAlpha", IrType::U32()},
       IrType::Member{"coefficients", kernelType}}));
  e.ok(builder.addTexture2d(0, BindingIndex(ConvolveMatrixBinding::InputTexture), "inputTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, BindingIndex(ConvolveMatrixBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba32Float));
  e.ok(builder.addReadOnlyStorageBuffer(0, BindingIndex(ConvolveMatrixBinding::Params), "params",
                                        paramsType));
  e.ok(AddSampleEdge(builder));

  auto entry = builder.createComputeEntryPoint(
      RcString(kConvolveMatrixEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kConvolveMatrixWorkgroupSize, kConvolveMatrixWorkgroupSize, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  FunctionBuilder fn = std::move(entry).result();
  const IrExpr gid = e(fn.ref("gid"));
  const IrExpr input = e(fn.ref("inputTexture"));
  const IrExpr output = e(fn.ref("outputTexture"));
  const IrExpr params = e(fn.ref("params"));
  const IrExpr extent =
      e(fn.addLet("extent", e(CallBuiltin(BuiltinFn::TextureDimensions, {output}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(extent, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(extent, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr coord = e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const IrExpr size = e(fn.addLet(
      "size", e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions, {input}))))));
  const IrExpr sumRgb = e(fn.addVar("sumRgb", IrType::Vec3f(), Splat(e, IrType::Vec3f(), 0.0f)));
  const IrExpr sumAlpha = e(fn.addVar("sumAlpha", IrType::F32(), LiteralF32(0.0f)));

  const IrExpr j = e(fn.beginFor("j", LiteralI32(0)));
  e.ok(fn.forCondition(e(Lt(j, e(Member(params, "orderY"))))));
  e.ok(fn.forContinuing(j, e(Add(j, LiteralI32(1)))));
  const IrExpr i = e(fn.beginFor("i", LiteralI32(0)));
  e.ok(fn.forCondition(e(Lt(i, e(Member(params, "orderX"))))));
  e.ok(fn.forContinuing(i, e(Add(i, LiteralI32(1)))));
  const IrExpr sourceCoord = e(fn.addLet(
      "sourceCoord", e(Add(coord, e(ConstructVector(IrType::Vec2i(),
                                                    {e(Sub(i, e(Member(params, "targetX")))),
                                                     e(Sub(j, e(Member(params, "targetY"))))}))))));
  const IrExpr source =
      e(fn.addLet("source", e(fn.callFunction("sampleEdge", {sourceCoord, size}))));
  const IrExpr kernelIndex = e(fn.addLet(
      "kernelIndex", e(Add(e(Mul(e(Sub(e(Sub(e(Member(params, "orderY")), LiteralI32(1))), j)),
                                 e(Member(params, "orderX")))),
                           e(Sub(e(Sub(e(Member(params, "orderX")), LiteralI32(1))), i))))));
  const IrExpr coefficient =
      e(fn.addLet("coefficient", e(Index(e(Member(params, "coefficients")), kernelIndex))));
  const IrExpr preserveAlpha = e(Eq(e(Member(params, "preserveAlpha")), LiteralU32(1)));
  e.ok(fn.beginIf(e(And(preserveAlpha, e(Gt(e(Swizzle(source, "w")), LiteralF32(0.0f)))))));
  e.ok(fn.assign(sumRgb,
                 e(Add(sumRgb, e(Mul(e(Div(e(Swizzle(source, "xyz")), e(Swizzle(source, "w")))),
                                     coefficient))))));
  e.ok(fn.elseBranch());
  e.ok(fn.assign(sumRgb, e(Add(sumRgb, e(Mul(e(Swizzle(source, "xyz")), coefficient))))));
  e.ok(fn.endIf());
  e.ok(fn.assign(sumAlpha, e(Add(sumAlpha, e(Mul(e(Swizzle(source, "w")), coefficient))))));
  e.ok(fn.endFor());
  e.ok(fn.endFor());

  const IrExpr sourceAlpha = e(fn.addLet(
      "sourceAlpha",
      e(Swizzle(e(CallBuiltin(BuiltinFn::TextureLoad, {input, coord, LiteralI32(0)})), "w"))));
  const IrExpr result = e(fn.addVar("result", IrType::Vec4f(), Splat(e, IrType::Vec4f(), 0.0f)));
  const IrExpr divisor = e(Member(params, "divisor"));
  const IrExpr bias = e(Member(params, "bias"));
  e.ok(fn.beginIf(preserveAlpha));
  const IrExpr straightRgb = e(fn.addLet(
      "straightRgb",
      e(CallBuiltin(BuiltinFn::Clamp,
                    {e(Add(e(Div(sumRgb, divisor)), e(ConstructVector(IrType::Vec3f(), {bias})))),
                     Splat(e, IrType::Vec3f(), 0.0f), Splat(e, IrType::Vec3f(), 1.0f)}))));
  e.ok(fn.assign(result, e(ConstructVector(IrType::Vec4f(),
                                           {e(Mul(straightRgb, sourceAlpha)), sourceAlpha}))));
  e.ok(fn.elseBranch());
  const IrExpr biased = e(fn.addLet("biased", e(Mul(bias, sourceAlpha))));
  const IrExpr outputAlpha = e(fn.addLet(
      "outputAlpha", e(CallBuiltin(BuiltinFn::Clamp, {e(Add(e(Div(sumAlpha, divisor)), biased)),
                                                      LiteralF32(0.0f), LiteralF32(1.0f)}))));
  const IrExpr outputRgb = e(fn.addLet(
      "outputRgb",
      e(CallBuiltin(
          BuiltinFn::Clamp,
          {e(Add(e(Div(sumRgb, divisor)), e(ConstructVector(IrType::Vec3f(), {biased})))),
           Splat(e, IrType::Vec3f(), 0.0f), e(ConstructVector(IrType::Vec3f(), {outputAlpha}))}))));
  e.ok(fn.assign(result, e(ConstructVector(IrType::Vec4f(), {outputRgb, outputAlpha}))));
  e.ok(fn.endIf());
  e.ok(fn.textureStore(output, coord,
                       e(CallBuiltin(BuiltinFn::Clamp, {result, Splat(e, IrType::Vec4f(), 0.0f),
                                                        Splat(e, IrType::Vec4f(), 1.0f)}))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
