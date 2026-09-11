#include "donner/gpu/shader/programs/Turbulence.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {
namespace {

uint32_t BindingIndex(TurbulenceBinding binding) {
  return static_cast<uint32_t>(binding);
}

IrExpr Four(ErrorLatch& e, float value) {
  return e(ConstructVector(IrType::Vec4f(), {LiteralF32(value)}));
}

IrExpr WrapTableIndex(ErrorLatch& e, const IrExpr& value) {
  const IrExpr size = LiteralI32(static_cast<int32_t>(kTurbulenceBaseTableSize));
  return e(Mod(e(Add(e(Mod(value, size)), size)), size));
}

ShaderStatus AddSCurve(ModuleBuilder& builder) {
  ErrorLatch e;
  auto created = builder.createFunction("s_curve", {{"t", IrType::F32()}}, IrType::F32());
  if (created.hasError()) {
    return std::move(created).error();
  }
  FunctionBuilder fn = std::move(created).result();
  const IrExpr t = e(fn.ref("t"));
  e.ok(fn.returnValue(e(Mul(e(Mul(t, t)), e(Sub(LiteralF32(3.0f), e(Mul(LiteralF32(2.0f), t))))))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddLerp(ModuleBuilder& builder) {
  ErrorLatch e;
  auto created = builder.createFunction(
      "lerp_f", {{"t", IrType::F32()}, {"a", IrType::F32()}, {"b", IrType::F32()}}, IrType::F32());
  if (created.hasError()) {
    return std::move(created).error();
  }
  FunctionBuilder fn = std::move(created).result();
  const IrExpr t = e(fn.ref("t"));
  const IrExpr a = e(fn.ref("a"));
  const IrExpr b = e(fn.ref("b"));
  e.ok(fn.returnValue(e(Add(a, e(Mul(t, e(Sub(b, a))))))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddNoise(ModuleBuilder& builder) {
  ErrorLatch e;
  auto created = builder.createFunction("noise2",
                                        {{"channel", IrType::I32()},
                                         {"x", IrType::F32()},
                                         {"y", IrType::F32()},
                                         {"stitchWidth", IrType::I32()},
                                         {"stitchHeight", IrType::I32()},
                                         {"wrapX", IrType::I32()},
                                         {"wrapY", IrType::I32()},
                                         {"stitchTiles", IrType::U32()}},
                                        IrType::F32());
  if (created.hasError()) {
    return std::move(created).error();
  }
  FunctionBuilder fn = std::move(created).result();

  const IrExpr tx = e(fn.addLet("tx", e(Add(e(fn.ref("x")), LiteralF32(4096.0f)))));
  const IrExpr bx0 = e(fn.addVar("bx0", IrType::I32(), e(Convert(IrType::I32(), tx))));
  const IrExpr bx1 = e(fn.addVar("bx1", IrType::I32(), e(Add(bx0, LiteralI32(1)))));
  const IrExpr rx0 = e(fn.addLet("rx0", e(Sub(tx, e(Convert(IrType::F32(), bx0))))));
  const IrExpr rx1 = e(fn.addLet("rx1", e(Sub(rx0, LiteralF32(1.0f)))));

  const IrExpr ty = e(fn.addLet("ty", e(Add(e(fn.ref("y")), LiteralF32(4096.0f)))));
  const IrExpr by0 = e(fn.addVar("by0", IrType::I32(), e(Convert(IrType::I32(), ty))));
  const IrExpr by1 = e(fn.addVar("by1", IrType::I32(), e(Add(by0, LiteralI32(1)))));
  const IrExpr ry0 = e(fn.addLet("ry0", e(Sub(ty, e(Convert(IrType::F32(), by0))))));
  const IrExpr ry1 = e(fn.addLet("ry1", e(Sub(ry0, LiteralF32(1.0f)))));

  e.ok(fn.beginIf(e(Eq(e(fn.ref("stitchTiles")), LiteralU32(1)))));
  const auto wrap = [&](const IrExpr& value, const char* wrapName, const char* sizeName) {
    e.ok(fn.beginIf(e(Ge(value, e(fn.ref(wrapName))))));
    e.ok(fn.assign(value, e(Sub(value, e(fn.ref(sizeName))))));
    e.ok(fn.endIf());
  };
  wrap(bx0, "wrapX", "stitchWidth");
  wrap(bx1, "wrapX", "stitchWidth");
  wrap(by0, "wrapY", "stitchHeight");
  wrap(by1, "wrapY", "stitchHeight");
  e.ok(fn.endIf());

  e.ok(fn.assign(bx0, WrapTableIndex(e, bx0)));
  e.ok(fn.assign(bx1, WrapTableIndex(e, bx1)));
  e.ok(fn.assign(by0, WrapTableIndex(e, by0)));
  e.ok(fn.assign(by1, WrapTableIndex(e, by1)));

  const IrExpr lattice = e(Member(e(fn.ref("tables")), "lattice"));
  const auto latticeAt = [&](const IrExpr& index) { return e(Index(lattice, index)); };
  const IrExpr iValue = e(fn.addLet("iValue", latticeAt(bx0)));
  const IrExpr jValue = e(fn.addLet("jValue", latticeAt(bx1)));
  const IrExpr b00 = e(fn.addLet("b00", latticeAt(e(Add(iValue, by0)))));
  const IrExpr b10 = e(fn.addLet("b10", latticeAt(e(Add(jValue, by0)))));
  const IrExpr b01 = e(fn.addLet("b01", latticeAt(e(Add(iValue, by1)))));
  const IrExpr b11 = e(fn.addLet("b11", latticeAt(e(Add(jValue, by1)))));

  const IrExpr sx = e(fn.addLet("sx", e(fn.callFunction("s_curve", {rx0}))));
  const IrExpr sy = e(fn.addLet("sy", e(fn.callFunction("s_curve", {ry0}))));
  const IrExpr channelOffset = e(fn.addLet(
      "channelOffset",
      e(Mul(e(fn.ref("channel")), LiteralI32(static_cast<int32_t>(kTurbulenceTableSize))))));
  const IrExpr gradX = e(Member(e(fn.ref("tables")), "gradX"));
  const IrExpr gradY = e(Member(e(fn.ref("tables")), "gradY"));
  const auto gradient = [&](const IrExpr& table, const IrExpr& latticeIndex) {
    return e(Index(table, e(Add(channelOffset, latticeIndex))));
  };
  const auto dot = [&](const IrExpr& latticeIndex, const IrExpr& x, const IrExpr& y) {
    return e(
        Add(e(Mul(gradient(gradX, latticeIndex), x)), e(Mul(gradient(gradY, latticeIndex), y))));
  };

  const IrExpr u0 = e(fn.addLet("u0", dot(b00, rx0, ry0)));
  const IrExpr v0 = e(fn.addLet("v0", dot(b10, rx1, ry0)));
  const IrExpr aValue = e(fn.addLet("aValue", e(fn.callFunction("lerp_f", {sx, u0, v0}))));
  const IrExpr u1 = e(fn.addLet("u1", dot(b01, rx0, ry1)));
  const IrExpr v1 = e(fn.addLet("v1", dot(b11, rx1, ry1)));
  const IrExpr bValue = e(fn.addLet("bValue", e(fn.callFunction("lerp_f", {sx, u1, v1}))));
  e.ok(fn.returnValue(e(fn.callFunction("lerp_f", {sy, aValue, bValue}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

IrExpr Noise4(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& x, const IrExpr& y,
              const IrExpr& stitchWidth, const IrExpr& stitchHeight, const IrExpr& wrapX,
              const IrExpr& wrapY, const IrExpr& stitchTiles) {
  std::vector<IrExpr> channels;
  for (int32_t channel = 0; channel < 4; ++channel) {
    channels.push_back(e(fn.callFunction("noise2", {LiteralI32(channel), x, y, stitchWidth,
                                                    stitchHeight, wrapX, wrapY, stitchTiles})));
  }
  return e(ConstructVector(IrType::Vec4f(), std::move(channels)));
}

}  // namespace

ShaderResult<IrModule> BuildTurbulenceModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  const IrType paramsType =
      e(IrType::Struct("TurbulenceParams", {{"baseFreqX", IrType::F32()},
                                            {"baseFreqY", IrType::F32()},
                                            {"numOctaves", IrType::I32()},
                                            {"seed", IrType::I32()},
                                            {"stitchTiles", IrType::U32()},
                                            {"typeFlag", IrType::U32()},
                                            {"tileWidth", IrType::F32()},
                                            {"tileHeight", IrType::F32()},
                                            {"filterFromDeviceA", IrType::F32()},
                                            {"filterFromDeviceB", IrType::F32()},
                                            {"filterFromDeviceC", IrType::F32()},
                                            {"filterFromDeviceD", IrType::F32()}}));
  const IrType latticeType = e(IrType::SizedArray(IrType::I32(), kTurbulenceTableSize));
  const IrType gradientType = e(IrType::SizedArray(IrType::F32(), kTurbulenceGradientTableSize));
  const IrType tablesType = e(
      IrType::Struct("TurbulenceTables",
                     {{"lattice", latticeType}, {"gradX", gradientType}, {"gradY", gradientType}}));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, BindingIndex(TurbulenceBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba32Float));
  e.ok(builder.addReadOnlyStorageBuffer(0, BindingIndex(TurbulenceBinding::Params), "params",
                                        paramsType));
  e.ok(builder.addReadOnlyStorageBuffer(0, BindingIndex(TurbulenceBinding::Tables), "tables",
                                        tablesType));
  e.ok(AddSCurve(builder));
  e.ok(AddLerp(builder));
  e.ok(AddNoise(builder));

  auto created = builder.createComputeEntryPoint(
      RcString(kTurbulenceEntryPoint),
      {{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt, BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kTurbulenceWorkgroupSize, kTurbulenceWorkgroupSize, 1});
  if (created.hasError()) {
    return std::move(created).error();
  }
  FunctionBuilder fn = std::move(created).result();
  const IrExpr output = e(fn.ref("outputTexture"));
  const IrExpr params = e(fn.ref("params"));
  const IrExpr gid = e(fn.ref("gid"));
  const IrExpr extent =
      e(fn.addLet("extent", e(CallBuiltin(BuiltinFn::TextureDimensions, {output}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(extent, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(extent, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr coord = e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const IrExpr px = e(fn.addLet("px", e(Convert(IrType::F32(), e(Swizzle(coord, "x"))))));
  const IrExpr py = e(fn.addLet("py", e(Convert(IrType::F32(), e(Swizzle(coord, "y"))))));
  const IrExpr ux = e(fn.addLet("ux", e(Add(e(Mul(e(Member(params, "filterFromDeviceA")), px)),
                                            e(Mul(e(Member(params, "filterFromDeviceB")), py))))));
  const IrExpr uy = e(fn.addLet("uy", e(Add(e(Mul(e(Member(params, "filterFromDeviceC")), px)),
                                            e(Mul(e(Member(params, "filterFromDeviceD")), py))))));

  const IrExpr pixel = e(fn.addVar("pixel", IrType::Vec4f(), Four(e, 0.0f)));
  const IrExpr frequencyX =
      e(fn.addVar("frequencyX", IrType::F32(), e(Member(params, "baseFreqX"))));
  const IrExpr frequencyY =
      e(fn.addVar("frequencyY", IrType::F32(), e(Member(params, "baseFreqY"))));
  const IrExpr inverseRatio = e(fn.addVar("inverseRatio", IrType::F32(), LiteralF32(1.0f)));

  const IrExpr octave = e(fn.beginFor("octave", LiteralI32(0)));
  e.ok(fn.forCondition(e(Lt(octave, e(Member(params, "numOctaves"))))));
  e.ok(fn.forContinuing(octave, e(Add(octave, LiteralI32(1)))));
  const IrExpr nx = e(fn.addLet("nx", e(Mul(ux, frequencyX))));
  const IrExpr ny = e(fn.addLet("ny", e(Mul(uy, frequencyY))));
  const IrExpr stitchWidth =
      e(fn.addVar("stitchWidth", IrType::I32(),
                  e(Convert(IrType::I32(), e(Mul(e(Member(params, "tileWidth")), frequencyX))))));
  const IrExpr stitchHeight =
      e(fn.addVar("stitchHeight", IrType::I32(),
                  e(Convert(IrType::I32(), e(Mul(e(Member(params, "tileHeight")), frequencyY))))));
  e.ok(fn.beginIf(e(Lt(stitchWidth, LiteralI32(1)))));
  e.ok(fn.assign(stitchWidth, LiteralI32(1)));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Lt(stitchHeight, LiteralI32(1)))));
  e.ok(fn.assign(stitchHeight, LiteralI32(1)));
  e.ok(fn.endIf());
  const IrExpr wrapX = e(fn.addLet("wrapX", e(Add(stitchWidth, LiteralI32(4096)))));
  const IrExpr wrapY = e(fn.addLet("wrapY", e(Add(stitchHeight, LiteralI32(4096)))));
  const IrExpr noise = e(fn.addVar("noise", IrType::Vec4f(),
                                   Noise4(e, fn, nx, ny, stitchWidth, stitchHeight, wrapX, wrapY,
                                          e(Member(params, "stitchTiles")))));
  e.ok(fn.beginIf(e(Eq(e(Member(params, "typeFlag")), LiteralU32(1)))));
  e.ok(fn.assign(noise, e(CallBuiltin(BuiltinFn::Abs, {noise}))));
  e.ok(fn.endIf());
  e.ok(fn.assign(pixel, e(Add(pixel, e(Mul(noise, inverseRatio))))));
  e.ok(fn.assign(frequencyX, e(Mul(frequencyX, LiteralF32(2.0f)))));
  e.ok(fn.assign(frequencyY, e(Mul(frequencyY, LiteralF32(2.0f)))));
  e.ok(fn.assign(inverseRatio, e(Mul(inverseRatio, LiteralF32(0.5f)))));
  e.ok(fn.endFor());

  const IrExpr rgba = e(fn.addVar("rgba", IrType::Vec4f(), Four(e, 0.0f)));
  e.ok(fn.beginIf(e(Eq(e(Member(params, "typeFlag")), LiteralU32(0)))));
  e.ok(fn.assign(
      rgba, e(CallBuiltin(BuiltinFn::Clamp, {e(Mul(e(Add(pixel, Four(e, 1.0f))), Four(e, 0.5f))),
                                             Four(e, 0.0f), Four(e, 1.0f)}))));
  e.ok(fn.elseBranch());
  e.ok(fn.assign(rgba, e(CallBuiltin(BuiltinFn::Clamp, {pixel, Four(e, 0.0f), Four(e, 1.0f)}))));
  e.ok(fn.endIf());
  const IrExpr alpha = e(Swizzle(rgba, "w"));
  const IrExpr result =
      e(ConstructVector(IrType::Vec4f(), {e(Mul(e(Swizzle(rgba, "xyz")), alpha)), alpha}));
  e.ok(fn.textureStore(output, coord, result));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
