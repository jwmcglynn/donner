#include "donner/gpu/shader/programs/Flood.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {

namespace {

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(FloodBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Adds the two module-scope bindings the entry point writes and reads.
ShaderStatus AddBindings(ModuleBuilder& builder, const IrType& paramsType) {
  if (ShaderStatus status =
          builder.addWriteOnlyStorageTexture2d(0, BindingIndex(FloodBinding::OutputTexture),
                                               "outputTexture", StorageTextureFormat::Rgba8Unorm);
      status.hasError()) {
    return status;
  }
  return builder.addUniformBuffer(0, BindingIndex(FloodBinding::Params), "params", paramsType);
}

}  // namespace

ShaderResult<IrModule> BuildFloodModule() {
  ErrorLatch e;
  ModuleBuilder builder;

  const IrType paramsType =
      e(IrType::Struct("FloodParams", {IrType::Member{"color", IrType::Vec4f()}}));
  e.ok(AddBindings(builder, paramsType));

  auto entryResult =
      builder.createComputeEntryPoint("cs_main",
                                      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
                                               BuiltinInput::GlobalInvocationId}},
                                      WorkgroupSize{kFloodWorkgroupSize, kFloodWorkgroupSize, 1});
  if (entryResult.hasError()) {
    return std::move(entryResult).error();
  }
  FunctionBuilder fn = std::move(entryResult).result();

  const IrExpr gid = e(fn.ref("gid"));
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
  // The color is stored exactly as the uniform carries it. Whether it is premultiplied is the
  // caller's decision, and nothing here re-associates it with the alpha channel.
  e.ok(fn.textureStore(outputTexture, coords, e(Member(e(fn.ref("params")), "color"))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
