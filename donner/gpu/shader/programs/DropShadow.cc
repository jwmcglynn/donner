#include "donner/gpu/shader/programs/DropShadow.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
#include "donner/gpu/shader/programs/RoundHalfAwayFromZero.h"
namespace donner::gpu::shader::programs {
ShaderResult<IrModule> BuildDropShadowModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  const auto paramsType = e(
      IrType::Struct("DropShadowParams",
                     {IrType::Member{"color", IrType::Vec4f()}, IrType::Member{"dx", IrType::F32()},
                      IrType::Member{"dy", IrType::F32()}, IrType::Member{"pad0", IrType::U32()},
                      IrType::Member{"pad1", IrType::U32()}}));
  e.ok(builder.addTexture2d(0, static_cast<uint32_t>(DropShadowBinding::SourceTexture),
                            "sourceTexture"));
  e.ok(builder.addTexture2d(0, static_cast<uint32_t>(DropShadowBinding::BlurredTexture),
                            "blurredTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0,
                                            static_cast<uint32_t>(DropShadowBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba32Float));
  e.ok(builder.addUniformBuffer(0, static_cast<uint32_t>(DropShadowBinding::Params), "params",
                                paramsType));
  e.ok(AddRoundHalfAwayFromZero(builder));
  auto entry = builder.createComputeEntryPoint(
      RcString(kDropShadowEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kDropShadowWorkgroupSize, kDropShadowWorkgroupSize, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  auto fn = std::move(entry).result();
  const auto output = e(fn.ref("outputTexture")), blurred = e(fn.ref("blurredTexture"));
  const auto gid = e(fn.ref("gid")), params = e(fn.ref("params"));
  const auto extent =
      e(fn.addLet("extent", e(CallBuiltin(BuiltinFn::TextureDimensions, {output}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(extent, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(extent, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  const auto coord = e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const auto round = [&](const char* axis) {
    return e(Convert(IrType::I32(), e(fn.callFunction(RcString(kRoundHalfAwayFromZeroName),
                                                      {e(Member(params, axis))}))));
  };
  const auto sample =
      e(fn.addLet("sampleCoord",
                  e(Sub(coord, e(ConstructVector(IrType::Vec2i(), {round("dx"), round("dy")}))))));
  const auto size = e(fn.addLet(
      "blurredSize",
      e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions, {blurred}))))));
  const auto zero = e(ConstructVector(IrType::Vec2i(), {LiteralI32(0)}));
  const auto alpha = e(fn.addVar("blurredAlpha", IrType::F32(), LiteralF32(0)));
  e.ok(fn.beginIf(e(And(e(CallBuiltin(BuiltinFn::All, {e(Ge(sample, zero))})),
                        e(CallBuiltin(BuiltinFn::All, {e(Lt(sample, size))}))))));
  e.ok(fn.assign(
      alpha,
      e(Swizzle(e(CallBuiltin(BuiltinFn::TextureLoad, {blurred, sample, LiteralI32(0)})), "w"))));
  e.ok(fn.endIf());
  const auto color = e(Member(params, "color"));
  const auto floodAlpha = e(Swizzle(color, "w"));
  const auto shadow = e(fn.addLet(
      "shadow", e(ConstructVector(IrType::Vec4f(),
                                  {e(Mul(e(Mul(e(Swizzle(color, "xyz")), floodAlpha)), alpha)),
                                   e(Mul(floodAlpha, alpha))}))));
  const auto top = e(fn.addLet(
      "top",
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("sourceTexture")), coord, LiteralI32(0)}))));
  const auto result = e(fn.addLet(
      "result", e(Add(top, e(Mul(shadow, e(Sub(LiteralF32(1), e(Swizzle(top, "w"))))))))));
  const auto low = e(ConstructVector(IrType::Vec4f(), {LiteralF32(0)}));
  const auto high = e(ConstructVector(IrType::Vec4f(), {LiteralF32(1)}));
  e.ok(fn.textureStore(output, coord, e(CallBuiltin(BuiltinFn::Clamp, {result, low, high}))));
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}
}  // namespace donner::gpu::shader::programs
