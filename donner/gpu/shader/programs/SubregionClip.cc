#include "donner/gpu/shader/programs/SubregionClip.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {

namespace {

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(SubregionClipBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Adds the three module-scope bindings the entry point reads and writes.
ShaderStatus AddBindings(ModuleBuilder& builder, const IrType& paramsType) {
  if (ShaderStatus status =
          builder.addTexture2d(0, BindingIndex(SubregionClipBinding::InputTexture), "inputTexture");
      status.hasError()) {
    return status;
  }
  if (ShaderStatus status =
          builder.addWriteOnlyStorageTexture2d(0, BindingIndex(SubregionClipBinding::OutputTexture),
                                               "outputTexture", StorageTextureFormat::Rgba8Unorm);
      status.hasError()) {
    return status;
  }
  return builder.addUniformBuffer(0, BindingIndex(SubregionClipBinding::Params), "params",
                                  paramsType);
}

}  // namespace

ShaderResult<IrModule> BuildSubregionClipModule() {
  ErrorLatch e;
  ModuleBuilder builder;

  const IrType f32 = IrType::F32();
  const IrType paramsType = e(IrType::Struct(
      "SubregionClipParams",
      {IrType::Member{"invA", f32}, IrType::Member{"invB", f32}, IrType::Member{"invC", f32},
       IrType::Member{"invD", f32}, IrType::Member{"invE", f32}, IrType::Member{"invF", f32},
       IrType::Member{"userX0", f32}, IrType::Member{"userY0", f32}, IrType::Member{"userX1", f32},
       IrType::Member{"userY1", f32}}));
  e.ok(AddBindings(builder, paramsType));

  auto entryResult = builder.createComputeEntryPoint(
      "cs_main",
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kSubregionClipWorkgroupSize, kSubregionClipWorkgroupSize, 1});
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

  // The rectangle is axis-aligned in user space, so the test runs on the pixel center mapped back
  // through the inverse transform rather than on the pixel index. A rotated transform makes those
  // two different regions.
  const IrExpr params = e(fn.ref("params"));
  const IrExpr centerX = e(fn.addLet(
      "centerX", e(Add(e(Convert(IrType::F32(), e(Swizzle(coords, "x")))), LiteralF32(0.5f)))));
  const IrExpr centerY = e(fn.addLet(
      "centerY", e(Add(e(Convert(IrType::F32(), e(Swizzle(coords, "y")))), LiteralF32(0.5f)))));
  const IrExpr userX =
      e(fn.addLet("userX", e(Add(e(Add(e(Mul(e(Member(params, "invA")), centerX)),
                                       e(Mul(e(Member(params, "invC")), centerY)))),
                                 e(Member(params, "invE"))))));
  const IrExpr userY =
      e(fn.addLet("userY", e(Add(e(Add(e(Mul(e(Member(params, "invB")), centerX)),
                                       e(Mul(e(Member(params, "invD")), centerY)))),
                                 e(Member(params, "invF"))))));

  // Half-open on both axes, so subregions that share an edge neither overlap nor leave a seam.
  const IrExpr outside =
      e(fn.addLet("outside", e(Or(e(Or(e(Or(e(Lt(userX, e(Member(params, "userX0")))),
                                            e(Ge(userX, e(Member(params, "userX1")))))),
                                       e(Lt(userY, e(Member(params, "userY0")))))),
                                  e(Ge(userY, e(Member(params, "userY1"))))))));

  e.ok(fn.beginIf(outside));
  e.ok(fn.textureStore(outputTexture, coords,
                       e(ConstructVector(IrType::Vec4f(), {LiteralF32(0.0f), LiteralF32(0.0f),
                                                           LiteralF32(0.0f), LiteralF32(0.0f)}))));
  e.ok(fn.elseBranch());
  e.ok(fn.textureStore(
      outputTexture, coords,
      e(CallBuiltin(BuiltinFn::TextureLoad, {inputTexture, coords, LiteralI32(0)}))));
  e.ok(fn.endIf());
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
