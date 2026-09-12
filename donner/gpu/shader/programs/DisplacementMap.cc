#include "donner/gpu/shader/programs/DisplacementMap.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {
namespace {

uint32_t BindingIndex(DisplacementMapBinding binding) {
  return static_cast<uint32_t>(binding);
}

ShaderStatus AddChannel(ModuleBuilder& builder) {
  ErrorLatch e;
  auto result = builder.createFunction(
      "channel", {{"color", IrType::Vec4f()}, {"selector", IrType::U32()}}, IrType::F32());
  if (result.hasError()) {
    return std::move(result).error();
  }
  auto fn = std::move(result).result();
  const auto color = e(fn.ref("color"));
  const auto alpha = e(Swizzle(color, "w"));
  const auto selector = e(fn.ref("selector"));
  e.ok(fn.beginIf(e(Eq(selector, LiteralU32(3)))));
  e.ok(fn.returnValue(alpha));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Le(alpha, LiteralF32(0)))));
  e.ok(fn.returnValue(LiteralF32(0)));
  e.ok(fn.endIf());
  const auto value = e(fn.addVar("value", IrType::F32(), alpha));
  for (const auto& [index, name] : {std::pair{0u, "x"}, std::pair{1u, "y"}, std::pair{2u, "z"}}) {
    e.ok(fn.beginIf(e(Eq(selector, LiteralU32(index)))));
    e.ok(fn.assign(value, e(Swizzle(color, name))));
    e.ok(fn.endIf());
  }
  e.ok(fn.returnValue(e(
      CallBuiltin(BuiltinFn::Min, {LiteralF32(1), e(Mul(value, e(Div(LiteralF32(1), alpha))))}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddSample(ModuleBuilder& builder) {
  ErrorLatch e;
  auto result = builder.createFunction(
      "sampleSource", {{"coord", IrType::Vec2i()}, {"size", IrType::Vec2i()}}, IrType::Vec4f());
  if (result.hasError()) {
    return std::move(result).error();
  }
  auto fn = std::move(result).result();
  const auto coord = e(fn.ref("coord"));
  const auto size = e(fn.ref("size"));
  const auto zero = e(ConstructVector(IrType::Vec2i(), {LiteralI32(0)}));
  e.ok(fn.beginIf(e(Or(e(CallBuiltin(BuiltinFn::Any, {e(Lt(coord, zero))})),
                       e(CallBuiltin(BuiltinFn::Any, {e(Ge(coord, size))}))))));
  e.ok(fn.returnValue(e(ConstructVector(IrType::Vec4f(), {LiteralF32(0)}))));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("sourceTexture")), coord, LiteralI32(0)}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

IrExpr Interpolate(ErrorLatch& e, const IrExpr& first, const IrExpr& second,
                   const IrExpr& fraction) {
  return e(Add(first, e(Mul(fraction, e(Sub(second, first))))));
}

void AddSampling(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& coord, const IrExpr& size) {
  const auto params = e(fn.ref("params"));
  const auto map = e(fn.addLet(
      "map",
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("mapTexture")), coord, LiteralI32(0)}))));
  const auto cx = e(fn.callFunction("channel", {map, e(Member(params, "xChannel"))}));
  const auto cy = e(fn.callFunction("channel", {map, e(Member(params, "yChannel"))}));
  const auto midpoint = e(ConstructVector(IrType::Vec2f(), {LiteralF32(0.5f)}));
  const auto delta = e(Mul(e(Sub(e(ConstructVector(IrType::Vec2f(), {cx, cy})), midpoint)),
                           e(Member(params, "scale"))));
  const auto position = e(fn.addLet("position", e(Add(e(Convert(IrType::Vec2f(), coord)), delta))));
  const auto base = e(
      fn.addLet("base", e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::Floor, {position}))))));
  const auto fraction =
      e(fn.addLet("fraction", e(Sub(position, e(Convert(IrType::Vec2f(), base))))));
  const auto sample = [&](int32_t x, int32_t y) {
    return e(fn.callFunction(
        "sampleSource",
        {e(Add(base, e(ConstructVector(IrType::Vec2i(), {LiteralI32(x), LiteralI32(y)})))), size}));
  };
  const auto top =
      e(fn.addLet("top", Interpolate(e, sample(0, 0), sample(1, 0), e(Swizzle(fraction, "x")))));
  const auto bottom =
      e(fn.addLet("bottom", Interpolate(e, sample(0, 1), sample(1, 1), e(Swizzle(fraction, "x")))));
  const auto output = Interpolate(e, top, bottom, e(Swizzle(fraction, "y")));
  const auto low = e(ConstructVector(IrType::Vec4f(), {LiteralF32(0)}));
  const auto high = e(ConstructVector(IrType::Vec4f(), {LiteralF32(1)}));
  e.ok(fn.textureStore(e(fn.ref("outputTexture")), coord,
                       e(CallBuiltin(BuiltinFn::Clamp, {output, low, high}))));
}

}  // namespace

ShaderResult<IrModule> BuildDisplacementMapModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  e.ok(builder.addTexture2d(0, BindingIndex(DisplacementMapBinding::SourceTexture),
                            "sourceTexture"));
  e.ok(builder.addTexture2d(0, BindingIndex(DisplacementMapBinding::MapTexture), "mapTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, BindingIndex(DisplacementMapBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba32Float));
  e.ok(builder.addUniformBuffer(
      0, BindingIndex(DisplacementMapBinding::Params), "params",
      e(IrType::Struct("DisplacementParams", {{"scale", IrType::F32()},
                                              {"xChannel", IrType::U32()},
                                              {"yChannel", IrType::U32()},
                                              {"padding", IrType::U32()}}))));
  e.ok(AddChannel(builder));
  e.ok(AddSample(builder));
  auto result = builder.createComputeEntryPoint(
      RcString(kDisplacementMapEntryPoint),
      {{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt, BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kDisplacementMapWorkgroupSize, kDisplacementMapWorkgroupSize, 1});
  if (result.hasError()) {
    return std::move(result).error();
  }
  auto fn = std::move(result).result();
  const auto coord =
      e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(e(fn.ref("gid")), "xy"))))));
  const auto size = e(
      fn.addLet("size", e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions,
                                                                 {e(fn.ref("outputTexture"))}))))));
  e.ok(fn.beginIf(e(CallBuiltin(BuiltinFn::Any, {e(Ge(coord, size))}))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  AddSampling(e, fn, coord, size);
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
