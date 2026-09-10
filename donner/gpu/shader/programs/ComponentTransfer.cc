#include "donner/gpu/shader/programs/ComponentTransfer.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
namespace donner::gpu::shader::programs {
namespace {
struct ChannelFunction {
  IrExpr kind, tableOffset, tableCount, slope, intercept, amplitude, exponent, offset;
};

IrExpr TableValue(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& index) {
  return e(Index(e(fn.ref("params")), e(Add(LiteralU32(32), index))));
}
void AddTable(ErrorLatch& e, FunctionBuilder& fn, const ChannelFunction& f, const IrExpr& c,
              const IrExpr& result) {
  const auto count = f.tableCount, offset = f.tableOffset;
  e.ok(fn.beginIf(e(Eq(f.kind, LiteralU32(1)))));
  e.ok(fn.beginIf(e(Eq(count, LiteralU32(1)))));
  e.ok(fn.assign(result, TableValue(e, fn, offset)));
  e.ok(fn.elseBranch());
  e.ok(fn.beginIf(e(Gt(count, LiteralU32(1)))));
  const auto position =
      e(fn.addLet("position", e(Mul(c, e(Convert(IrType::F32(), e(Sub(count, LiteralU32(1)))))))));
  const auto index =
      e(fn.addLet("index", e(CallBuiltin(BuiltinFn::Min, {e(Convert(IrType::U32(), position)),
                                                          e(Sub(count, LiteralU32(2)))}))));
  const auto fraction =
      e(fn.addLet("fraction", e(Sub(position, e(Convert(IrType::F32(), index))))));
  const auto first = TableValue(e, fn, e(Add(offset, index)));
  const auto second = TableValue(e, fn, e(Add(e(Add(offset, index)), LiteralU32(1))));
  e.ok(fn.assign(result,
                 e(Add(e(Mul(first, e(Sub(LiteralF32(1), fraction)))), e(Mul(second, fraction))))));
  e.ok(fn.endIf());
  e.ok(fn.endIf());
  e.ok(fn.endIf());
}
void AddDiscrete(ErrorLatch& e, FunctionBuilder& fn, const ChannelFunction& f, const IrExpr& c,
                 const IrExpr& result) {
  const auto count = f.tableCount;
  e.ok(fn.beginIf(e(And(e(Eq(f.kind, LiteralU32(2))), e(Gt(count, LiteralU32(0)))))));
  const auto index = e(fn.addLet(
      "discreteIndex",
      e(CallBuiltin(BuiltinFn::Min,
                    {e(Convert(IrType::U32(), e(Mul(c, e(Convert(IrType::F32(), count)))))),
                     e(Sub(count, LiteralU32(1)))}))));
  e.ok(fn.assign(result, TableValue(e, fn, e(Add(f.tableOffset, index)))));
  e.ok(fn.endIf());
}
void AddGamma(ErrorLatch& e, FunctionBuilder& fn, const ChannelFunction& f, const IrExpr& c,
              const IrExpr& result) {
  const auto amplitude = f.amplitude, exponent = f.exponent;
  const auto offset = f.offset;
  e.ok(fn.beginIf(e(Eq(f.kind, LiteralU32(4)))));
  e.ok(fn.beginIf(e(Eq(amplitude, LiteralF32(0)))));
  e.ok(fn.assign(result, offset));
  e.ok(fn.elseBranch());
  e.ok(fn.beginIf(e(Eq(exponent, LiteralF32(0)))));
  e.ok(fn.assign(result, e(Add(amplitude, offset))));
  e.ok(fn.elseBranch());
  e.ok(fn.beginIf(e(And(e(Eq(c, LiteralF32(0))), e(Lt(exponent, LiteralF32(0)))))));
  e.ok(fn.assign(result, e(CallBuiltin(BuiltinFn::Select, {LiteralF32(0), LiteralF32(1),
                                                           e(Gt(amplitude, LiteralF32(0)))}))));
  e.ok(fn.elseBranch());
  e.ok(fn.assign(result,
                 e(Add(e(Mul(amplitude, e(CallBuiltin(BuiltinFn::Pow, {c, exponent})))), offset))));
  e.ok(fn.endIf());
  e.ok(fn.endIf());
  e.ok(fn.endIf());
  e.ok(fn.endIf());
}
ShaderStatus AddTransfer(ModuleBuilder& builder) {
  ErrorLatch e;
  auto result = builder.createFunction(
      "transfer", {IrParam{"value", IrType::F32()}, IrParam{"channel", IrType::U32()}},
      IrType::F32());
  if (result.hasError()) {
    return std::move(result).error();
  }
  auto fn = std::move(result).result();
  const auto c = e(fn.addLet(
      "c", e(CallBuiltin(BuiltinFn::Clamp, {e(fn.ref("value")), LiteralF32(0), LiteralF32(1)}))));
  const auto base = e(fn.addLet("base", e(Mul(e(fn.ref("channel")), LiteralU32(8)))));
  const auto word = [&](uint32_t index) {
    return e(Index(e(fn.ref("params")), e(Add(base, LiteralU32(index)))));
  };
  const ChannelFunction f{e(Convert(IrType::U32(), word(0))),
                          e(Convert(IrType::U32(), word(1))),
                          e(Convert(IrType::U32(), word(2))),
                          word(3),
                          word(4),
                          word(5),
                          word(6),
                          word(7)};
  const auto output = e(fn.addVar("result", IrType::F32(), c));
  AddTable(e, fn, f, c, output);
  AddDiscrete(e, fn, f, c, output);
  e.ok(fn.beginIf(e(Eq(f.kind, LiteralU32(3)))));
  e.ok(fn.assign(output, e(Add(e(Mul(f.slope, c)), f.intercept))));
  e.ok(fn.endIf());
  AddGamma(e, fn, f, c, output);
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::Clamp, {output, LiteralF32(0), LiteralF32(1)}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}
}  // namespace
ShaderResult<IrModule> BuildComponentTransferModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  const auto paramsType = e(IrType::RuntimeArray(IrType::F32()));
  e.ok(builder.addTexture2d(0, 0, "inputTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, 1, "outputTexture",
                                            StorageTextureFormat::Rgba32Float));
  e.ok(builder.addReadOnlyStorageBuffer(0, 2, "params", paramsType));
  e.ok(AddTransfer(builder));
  auto entry = builder.createComputeEntryPoint(
      RcString(kComponentTransferEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kComponentTransferWorkgroupSize, kComponentTransferWorkgroupSize, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  auto fn = std::move(entry).result();
  const auto gid = e(fn.ref("gid")), output = e(fn.ref("outputTexture"));
  const auto extent =
      e(fn.addLet("extent", e(CallBuiltin(BuiltinFn::TextureDimensions, {output}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(extent, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(extent, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  const auto coord = e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const auto color = e(fn.addLet(
      "color",
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("inputTexture")), coord, LiteralI32(0)}))));
  const auto straight = e(
      fn.addVar("straight", IrType::Vec4f(), e(ConstructVector(IrType::Vec4f(), {LiteralF32(0)}))));
  const auto alpha = e(Swizzle(color, "w"));
  e.ok(fn.beginIf(e(Gt(alpha, LiteralF32(0)))));
  e.ok(fn.assign(straight, e(ConstructVector(IrType::Vec4f(),
                                             {e(Div(e(Swizzle(color, "xyz")), alpha)), alpha}))));
  e.ok(fn.endIf());
  const auto transfer = [&](const char* channel, uint32_t index) {
    return e(fn.callFunction("transfer", {e(Swizzle(straight, channel)), LiteralU32(index)}));
  };
  const auto transformed = e(fn.addLet(
      "transformed", e(ConstructVector(IrType::Vec4f(), {transfer("x", 0), transfer("y", 1),
                                                         transfer("z", 2), transfer("w", 3)}))));
  const auto transformedAlpha = e(Swizzle(transformed, "w"));
  e.ok(fn.textureStore(
      output, coord,
      e(ConstructVector(IrType::Vec4f(), {e(Mul(e(Swizzle(transformed, "xyz")), transformedAlpha)),
                                          transformedAlpha}))));
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}
}  // namespace donner::gpu::shader::programs
