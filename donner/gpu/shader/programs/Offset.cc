#include "donner/gpu/shader/programs/Offset.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
#include "donner/gpu/shader/programs/RoundHalfAwayFromZero.h"

namespace donner::gpu::shader::programs {

namespace {

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(OffsetBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Adds the three module-scope bindings the entry point reads and writes.
ShaderStatus AddBindings(ModuleBuilder& builder, const IrType& paramsType) {
  if (ShaderStatus status =
          builder.addTexture2d(0, BindingIndex(OffsetBinding::InputTexture), "inputTexture");
      status.hasError()) {
    return status;
  }
  if (ShaderStatus status =
          builder.addWriteOnlyStorageTexture2d(0, BindingIndex(OffsetBinding::OutputTexture),
                                               "outputTexture", StorageTextureFormat::Rgba8Unorm);
      status.hasError()) {
    return status;
  }
  return builder.addUniformBuffer(0, BindingIndex(OffsetBinding::Params), "params", paramsType);
}

}  // namespace

ShaderResult<IrModule> BuildOffsetModule() {
  ErrorLatch e;
  ModuleBuilder builder;

  const IrType f32 = IrType::F32();
  const IrType u32 = IrType::U32();
  const IrType paramsType =
      e(IrType::Struct("OffsetParams", {IrType::Member{"dx", f32}, IrType::Member{"dy", f32},
                                        // Two f32 members size the struct at 8 bytes. The
                                        // trailing words carry that to 16, which is the size a
                                        // host mirror declared with 16-byte alignment computes
                                        // for the same members.
                                        IrType::Member{"pad0", u32}, IrType::Member{"pad1", u32}}));
  e.ok(AddBindings(builder, paramsType));
  e.ok(AddRoundHalfAwayFromZero(builder));

  auto entryResult =
      builder.createComputeEntryPoint(RcString(kOffsetEntryPoint),
                                      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
                                               BuiltinInput::GlobalInvocationId}},
                                      WorkgroupSize{kOffsetWorkgroupSize, kOffsetWorkgroupSize, 1});
  if (entryResult.hasError()) {
    return std::move(entryResult).error();
  }
  FunctionBuilder fn = std::move(entryResult).result();

  const IrExpr gid = e(fn.ref("gid"));
  const IrExpr inputTexture = e(fn.ref("inputTexture"));
  const IrExpr outputTexture = e(fn.ref("outputTexture"));

  // Invocations past the destination edge return without writing, so a dispatch rounded up to
  // whole workgroups cannot store out of bounds.
  const IrExpr extent =
      e(fn.addLet("extent", e(CallBuiltin(BuiltinFn::TextureDimensions, {outputTexture}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(extent, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(extent, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr coords = e(fn.addLet("coords", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));

  // Sampling is texel-aligned, so a fractional shift has to become a whole number of pixels
  // before it is subtracted rather than after.
  const IrExpr params = e(fn.ref("params"));
  const RcString rounder{kRoundHalfAwayFromZeroName};
  const IrExpr shift = e(fn.addLet(
      "shift",
      e(ConstructVector(
          IrType::Vec2i(),
          {e(Convert(IrType::I32(), e(fn.callFunction(rounder, {e(Member(params, "dx"))})))),
           e(Convert(IrType::I32(), e(fn.callFunction(rounder, {e(Member(params, "dy"))}))))}))));
  const IrExpr source = e(fn.addLet("source", e(Sub(coords, shift))));

  // The bound on the source coordinate is the extent of the texture it indexes, which the guard
  // above cannot supply: that one bounds the destination.
  const IrExpr sourceExtent = e(fn.addLet(
      "sourceExtent",
      e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions, {inputTexture}))))));

  const IrExpr outside = e(fn.addLet(
      "outside", e(Or(e(Or(e(Or(e(Lt(e(Swizzle(source, "x")), LiteralI32(0))),
                                e(Lt(e(Swizzle(source, "y")), LiteralI32(0))))),
                           e(Ge(e(Swizzle(source, "x")), e(Swizzle(sourceExtent, "x")))))),
                      e(Ge(e(Swizzle(source, "y")), e(Swizzle(sourceExtent, "y"))))))));

  // Shifting exposes destination texels no source texel maps to, and the specification fills them
  // with transparent black rather than leaving them undefined.
  e.ok(fn.beginIf(outside));
  e.ok(fn.textureStore(outputTexture, coords,
                       e(ConstructVector(IrType::Vec4f(), {LiteralF32(0.0f), LiteralF32(0.0f),
                                                           LiteralF32(0.0f), LiteralF32(0.0f)}))));
  e.ok(fn.elseBranch());
  e.ok(fn.textureStore(
      outputTexture, coords,
      e(CallBuiltin(BuiltinFn::TextureLoad, {inputTexture, source, LiteralI32(0)}))));
  e.ok(fn.endIf());
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
