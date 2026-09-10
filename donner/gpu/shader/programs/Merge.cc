#include "donner/gpu/shader/programs/Merge.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {

ShaderResult<IrModule> BuildMergeModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  e.ok(
      builder.addTexture2d(0, static_cast<uint32_t>(MergeBinding::SourceTexture), "sourceTexture"));
  e.ok(builder.addTexture2d(0, static_cast<uint32_t>(MergeBinding::DestinationTexture),
                            "destinationTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, static_cast<uint32_t>(MergeBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba32Float));

  auto entry =
      builder.createComputeEntryPoint(RcString(kMergeEntryPoint),
                                      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
                                               BuiltinInput::GlobalInvocationId}},
                                      WorkgroupSize{kMergeWorkgroupSize, kMergeWorkgroupSize, 1});
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
  const IrExpr result =
      e(fn.addLet("result", e(Add(source, e(Mul(destination, remainingSourceAlpha))))));
  e.ok(fn.textureStore(outputTexture, coords, e(CallBuiltin(BuiltinFn::Saturate, {result}))));
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
