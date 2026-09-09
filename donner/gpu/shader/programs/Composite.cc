#include "donner/gpu/shader/programs/Composite.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {

ShaderResult<IrModule> BuildCompositeModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  e.ok(builder.addTexture2d(0, static_cast<uint32_t>(CompositeBinding::SourceTexture),
                            "sourceTexture"));
  e.ok(builder.addTexture2d(0, static_cast<uint32_t>(CompositeBinding::DestinationTexture),
                            "destinationTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0,
                                            static_cast<uint32_t>(CompositeBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba8Unorm));
  const IrType u32 = IrType::U32();
  const IrType f32 = IrType::F32();
  const IrType paramsType =
      e(IrType::Struct("CompositeParams", {IrType::Member{"op", u32}, IrType::Member{"pad0", u32},
                                           IrType::Member{"pad1", u32}, IrType::Member{"pad2", u32},
                                           IrType::Member{"k1", f32}, IrType::Member{"k2", f32},
                                           IrType::Member{"k3", f32}, IrType::Member{"k4", f32}}));
  e.ok(builder.addUniformBuffer(0, static_cast<uint32_t>(CompositeBinding::Params), "params",
                                paramsType));

  auto entry = builder.createComputeEntryPoint(
      RcString(kCompositeEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kCompositeWorkgroupSize, kCompositeWorkgroupSize, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  FunctionBuilder fn = std::move(entry).result();
  const IrExpr gid = e(fn.ref("gid"));
  const IrExpr outputTexture = e(fn.ref("outputTexture"));
  const IrExpr extent =
      e(fn.addLet("extent", e(CallBuiltin(BuiltinFn::TextureDimensions, {outputTexture}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(extent, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(extent, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr coords = e(fn.addLet("coords", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const IrExpr source = e(fn.addLet(
      "source",
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("sourceTexture")), coords, LiteralI32(0)}))));
  const IrExpr destination = e(fn.addLet(
      "destination", e(CallBuiltin(BuiltinFn::TextureLoad,
                                   {e(fn.ref("destinationTexture")), coords, LiteralI32(0)}))));
  const IrExpr remainingSourceAlpha =
      e(fn.addLet("remainingSourceAlpha", e(Sub(LiteralF32(1.0f), e(Swizzle(source, "w"))))));
  const IrExpr remainingDestinationAlpha = e(fn.addLet(
      "remainingDestinationAlpha", e(Sub(LiteralF32(1.0f), e(Swizzle(destination, "w"))))));
  const IrExpr params = e(fn.ref("params"));
  const IrExpr op = e(Member(params, "op"));
  const IrExpr result = e(fn.addVar("result", IrType::Vec4f(),
                                    e(Add(source, e(Mul(destination, remainingSourceAlpha))))));

  // Unknown operator values retain the SVG default, source-over.
  const auto apply = [&](CompositeOperator operation, const IrExpr& value) {
    e.ok(fn.beginIf(e(Eq(op, LiteralU32(static_cast<uint32_t>(operation))))));
    e.ok(fn.assign(result, value));
    e.ok(fn.endIf());
  };
  apply(CompositeOperator::In, e(Mul(source, e(Swizzle(destination, "w")))));
  apply(CompositeOperator::Out, e(Mul(source, remainingDestinationAlpha)));
  apply(CompositeOperator::Atop, e(Add(e(Mul(source, e(Swizzle(destination, "w")))),
                                       e(Mul(destination, remainingSourceAlpha)))));
  apply(CompositeOperator::Xor, e(Add(e(Mul(source, remainingDestinationAlpha)),
                                      e(Mul(destination, remainingSourceAlpha)))));
  apply(CompositeOperator::Lighter, e(Add(source, destination)));
  const IrExpr product = e(Mul(e(Mul(e(Member(params, "k1")), source)), destination));
  const IrExpr weightedSource = e(Mul(e(Member(params, "k2")), source));
  const IrExpr weightedDestination = e(Mul(e(Member(params, "k3")), destination));
  const IrExpr constant = e(ConstructVector(IrType::Vec4f(), {e(Member(params, "k4"))}));
  apply(CompositeOperator::Arithmetic,
        e(Add(e(Add(e(Add(product, weightedSource)), weightedDestination)), constant)));
  e.ok(fn.textureStore(outputTexture, coords, e(CallBuiltin(BuiltinFn::Saturate, {result}))));
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
