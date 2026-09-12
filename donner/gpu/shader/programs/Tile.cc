#include "donner/gpu/shader/programs/Tile.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
namespace donner::gpu::shader::programs {
namespace {
/// Wraps one signed coordinate into the positive source interval.
IrExpr WrappedAxis(ErrorLatch& e, const IrExpr& coord, const IrExpr& origin, const IrExpr& extent) {
  const IrExpr relative = e(Sub(coord, origin));
  return e(Add(e(Mod(e(Add(e(Mod(relative, extent)), extent)), extent)), origin));
}
}  // namespace
ShaderResult<IrModule> BuildTileModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  const auto binding = [](TileBinding value) { return static_cast<uint32_t>(value); };
  const IrType paramsType = e(IrType::Struct(
      "TileParams",
      {IrType::Member{"srcX", IrType::I32()}, IrType::Member{"srcY", IrType::I32()},
       IrType::Member{"srcW", IrType::I32()}, IrType::Member{"srcH", IrType::I32()}}));
  e.ok(builder.addTexture2d(0, binding(TileBinding::InputTexture), "inputTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, binding(TileBinding::OutputTexture), "outputTexture",
                                            StorageTextureFormat::Rgba32Float));
  e.ok(builder.addUniformBuffer(0, binding(TileBinding::Params), "params", paramsType));
  auto entry =
      builder.createComputeEntryPoint(RcString(kTileEntryPoint),
                                      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
                                               BuiltinInput::GlobalInvocationId}},
                                      WorkgroupSize{kTileWorkgroupSize, kTileWorkgroupSize, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  FunctionBuilder fn = std::move(entry).result();
  const IrExpr gid = e(fn.ref("gid"));
  const IrExpr input = e(fn.ref("inputTexture"));
  const IrExpr output = e(fn.ref("outputTexture"));
  const IrExpr params = e(fn.ref("params"));
  const IrExpr size = e(fn.addLet("size", e(CallBuiltin(BuiltinFn::TextureDimensions, {output}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(size, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(size, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  const IrExpr coord = e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const IrExpr width = e(Member(params, "srcW"));
  const IrExpr height = e(Member(params, "srcH"));
  e.ok(fn.beginIf(e(Or(e(Le(width, LiteralI32(0))), e(Le(height, LiteralI32(0)))))));
  e.ok(fn.textureStore(output, coord,
                       e(ConstructVector(IrType::Vec4f(), {LiteralF32(0), LiteralF32(0),
                                                           LiteralF32(0), LiteralF32(0)}))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  const IrExpr sample = e(
      fn.addLet("sampleCoord",
                e(ConstructVector(
                    IrType::Vec2i(),
                    {WrappedAxis(e, e(Swizzle(coord, "x")), e(Member(params, "srcX")), width),
                     WrappedAxis(e, e(Swizzle(coord, "y")), e(Member(params, "srcY")), height)}))));
  const IrExpr inputSize = e(fn.addLet(
      "inputSize",
      e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions, {input}))))));
  const IrExpr zero = e(ConstructVector(IrType::Vec2i(), {LiteralI32(0), LiteralI32(0)}));
  const IrExpr one = e(ConstructVector(IrType::Vec2i(), {LiteralI32(1), LiteralI32(1)}));
  const IrExpr clamped = e(fn.addLet(
      "clamped", e(CallBuiltin(BuiltinFn::Clamp, {sample, zero, e(Sub(inputSize, one))}))));
  e.ok(fn.textureStore(output, coord,
                       e(CallBuiltin(BuiltinFn::TextureLoad, {input, clamped, LiteralI32(0)}))));
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}
}  // namespace donner::gpu::shader::programs
