#include "donner/gpu/shader/programs/Morphology.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
namespace donner::gpu::shader::programs {
namespace {
IrExpr Four(ErrorLatch& e, float value) {
  return e(ConstructVector(IrType::Vec4f(), {LiteralF32(value), LiteralF32(value),
                                             LiteralF32(value), LiteralF32(value)}));
}
/// Tests a signed pixel against the source extent.
IrExpr Outside(ErrorLatch& e, const IrExpr& coord, const IrExpr& size) {
  return e(Or(e(Or(e(Lt(e(Swizzle(coord, "x")), LiteralI32(0))),
                   e(Lt(e(Swizzle(coord, "y")), LiteralI32(0))))),
              e(Or(e(Ge(e(Swizzle(coord, "x")), e(Swizzle(size, "x")))),
                   e(Ge(e(Swizzle(coord, "y")), e(Swizzle(size, "y"))))))));
}
}  // namespace
ShaderResult<IrModule> BuildMorphologyModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  const auto binding = [](MorphologyBinding value) { return static_cast<uint32_t>(value); };
  const IrType paramsType = e(IrType::Struct(
      "MorphologyParams",
      {IrType::Member{"radiusX", IrType::I32()}, IrType::Member{"radiusY", IrType::I32()},
       IrType::Member{"op", IrType::U32()}, IrType::Member{"pad", IrType::U32()}}));
  e.ok(builder.addTexture2d(0, binding(MorphologyBinding::InputTexture), "inputTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, binding(MorphologyBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba32Float));
  e.ok(builder.addUniformBuffer(0, binding(MorphologyBinding::Params), "params", paramsType));
  auto entry = builder.createComputeEntryPoint(
      RcString(kMorphologyEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kMorphologyWorkgroupSize, kMorphologyWorkgroupSize, 1});
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
  const IrExpr rx = e(Member(params, "radiusX"));
  const IrExpr ry = e(Member(params, "radiusY"));
  const IrExpr erode = e(fn.addLet("erode", e(Eq(e(Member(params, "op")), LiteralU32(0)))));
  e.ok(fn.beginIf(e(And(e(Le(rx, LiteralI32(0))), e(Le(ry, LiteralI32(0)))))));
  e.ok(fn.textureStore(output, coord,
                       e(CallBuiltin(BuiltinFn::TextureLoad, {input, coord, LiteralI32(0)}))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  const IrExpr accum = e(fn.addVar("accum", IrType::Vec4f(), Four(e, 0)));
  e.ok(fn.beginIf(erode));
  e.ok(fn.assign(accum, Four(e, 1)));
  e.ok(fn.endIf());
  const IrExpr dy = e(fn.beginFor("dy", e(Neg(ry))));
  e.ok(fn.forCondition(e(Le(dy, ry))));
  e.ok(fn.forContinuing(dy, e(Add(dy, LiteralI32(1)))));
  const IrExpr dx = e(fn.beginFor("dx", e(Neg(rx))));
  e.ok(fn.forCondition(e(Le(dx, rx))));
  e.ok(fn.forContinuing(dx, e(Add(dx, LiteralI32(1)))));
  const IrExpr source =
      e(fn.addLet("source", e(Add(coord, e(ConstructVector(IrType::Vec2i(), {dx, dy}))))));
  e.ok(fn.beginIf(Outside(e, source, size)));
  e.ok(fn.beginIf(erode));
  e.ok(fn.assign(accum, Four(e, 0)));
  e.ok(fn.endIf());
  e.ok(fn.elseBranch());
  const IrExpr sample = e(
      fn.addLet("sample", e(CallBuiltin(BuiltinFn::TextureLoad, {input, source, LiteralI32(0)}))));
  e.ok(fn.beginIf(erode));
  e.ok(fn.assign(accum, e(CallBuiltin(BuiltinFn::Min, {accum, sample}))));
  e.ok(fn.elseBranch());
  e.ok(fn.assign(accum, e(CallBuiltin(BuiltinFn::Max, {accum, sample}))));
  e.ok(fn.endIf());
  e.ok(fn.endIf());
  e.ok(fn.endFor());
  e.ok(fn.endFor());
  e.ok(fn.textureStore(output, coord, accum));
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}
}  // namespace donner::gpu::shader::programs
