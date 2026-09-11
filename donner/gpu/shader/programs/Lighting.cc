#include "donner/gpu/shader/programs/Lighting.h"

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {
namespace {

enum class LightingModel {
  Diffuse,
  Specular,
};

IrExpr Vec2i(ErrorLatch& e, const IrExpr& x, const IrExpr& y) {
  return e(ConstructVector(IrType::Vec2i(), {x, y}));
}

IrExpr Vec3f(ErrorLatch& e, const IrExpr& x, const IrExpr& y, const IrExpr& z) {
  return e(ConstructVector(IrType::Vec3f(), {x, y, z}));
}

IrExpr Vec4f(ErrorLatch& e, const IrExpr& x, const IrExpr& y, const IrExpr& z, const IrExpr& w) {
  return e(ConstructVector(IrType::Vec4f(), {x, y, z, w}));
}

IrExpr Zero3f(ErrorLatch& e) {
  return e(ConstructVector(IrType::Vec3f(), {LiteralF32(0.0f)}));
}

IrExpr ParamsMember(ErrorLatch& e, FunctionBuilder& fn, const char* name) {
  return e(Member(e(fn.ref("params")), name));
}

ShaderStatus AddSafeNormalize(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> result =
      builder.createFunction("safeNormalize", {IrParam{"value", IrType::Vec3f()}}, IrType::Vec3f());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr value = e(fn.ref("value"));
  const IrExpr lengthSquared =
      e(fn.addLet("lengthSquared", e(CallBuiltin(BuiltinFn::Dot, {value, value}))));
  e.ok(fn.beginIf(e(Gt(lengthSquared, LiteralF32(0.0f)))));
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::Normalize, {value}))));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(Zero3f(e)));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddSvgPow(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> result = builder.createFunction(
      "svgPow", {IrParam{"base", IrType::F32()}, IrParam{"exponent", IrType::F32()}},
      IrType::F32());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr base = e(fn.ref("base"));
  const IrExpr exponent = e(fn.ref("exponent"));
  e.ok(fn.beginIf(e(Eq(exponent, LiteralF32(0.0f)))));
  e.ok(fn.returnValue(LiteralF32(1.0f)));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::Pow, {base, exponent}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddHeightAt(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> result =
      builder.createFunction("heightAt", {IrParam{"coord", IrType::Vec2i()}}, IrType::F32());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr params = e(fn.ref("params"));
  const IrExpr low = Vec2i(e, e(Member(params, "sampleMinX")), e(Member(params, "sampleMinY")));
  const IrExpr high = Vec2i(e, e(Member(params, "sampleMaxX")), e(Member(params, "sampleMaxY")));
  const IrExpr clamped =
      e(fn.addLet("clamped", e(CallBuiltin(BuiltinFn::Clamp, {e(fn.ref("coord")), low, high}))));
  const IrExpr texel =
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("inputTexture")), clamped, LiteralI32(0)}));
  e.ok(fn.returnValue(e(Swizzle(texel, "w"))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddHorizontalDifference(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> result = builder.createFunction(
      "horizontalDifference", {IrParam{"coord", IrType::Vec2i()}}, IrType::F32());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr coord = e(fn.ref("coord"));
  const IrExpr x = e(Swizzle(coord, "x"));
  const IrExpr y = e(Swizzle(coord, "y"));
  const auto height = [&](const IrExpr& sampleX) {
    return e(fn.callFunction("heightAt", {Vec2i(e, sampleX, y)}));
  };
  e.ok(fn.beginIf(e(Eq(x, ParamsMember(e, fn, "sampleMinX")))));
  e.ok(fn.returnValue(e(Sub(height(e(Add(x, LiteralI32(1)))), height(x)))));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Eq(x, ParamsMember(e, fn, "sampleMaxX")))));
  e.ok(fn.returnValue(e(Sub(height(x), height(e(Sub(x, LiteralI32(1))))))));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(e(Sub(height(e(Add(x, LiteralI32(1)))), height(e(Sub(x, LiteralI32(1))))))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddVerticalDifference(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> result = builder.createFunction(
      "verticalDifference", {IrParam{"coord", IrType::Vec2i()}}, IrType::F32());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr coord = e(fn.ref("coord"));
  const IrExpr x = e(Swizzle(coord, "x"));
  const IrExpr y = e(Swizzle(coord, "y"));
  const auto height = [&](const IrExpr& sampleY) {
    return e(fn.callFunction("heightAt", {Vec2i(e, x, sampleY)}));
  };
  e.ok(fn.beginIf(e(Eq(y, ParamsMember(e, fn, "sampleMinY")))));
  e.ok(fn.returnValue(e(Sub(height(e(Add(y, LiteralI32(1)))), height(y)))));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Eq(y, ParamsMember(e, fn, "sampleMaxY")))));
  e.ok(fn.returnValue(e(Sub(height(y), height(e(Sub(y, LiteralI32(1))))))));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(e(Sub(height(e(Add(y, LiteralI32(1)))), height(e(Sub(y, LiteralI32(1))))))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddComputeNormal(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> result =
      builder.createFunction("computeNormal", {IrParam{"coord", IrType::Vec2i()}}, IrType::Vec3f());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr coord = e(fn.ref("coord"));
  const IrExpr x = e(Swizzle(coord, "x"));
  const IrExpr y = e(Swizzle(coord, "y"));
  const IrExpr minX = ParamsMember(e, fn, "sampleMinX");
  const IrExpr minY = ParamsMember(e, fn, "sampleMinY");
  const IrExpr maxX = ParamsMember(e, fn, "sampleMaxX");
  const IrExpr maxY = ParamsMember(e, fn, "sampleMaxY");
  const IrExpr nx =
      e(fn.addVar("nx", IrType::F32(),
                  e(Mul(LiteralF32(2.0f), e(fn.callFunction("horizontalDifference", {coord}))))));
  e.ok(fn.beginIf(e(Gt(y, minY))));
  e.ok(fn.assign(
      nx,
      e(Add(nx, e(fn.callFunction("horizontalDifference",
                                  {e(Add(coord, Vec2i(e, LiteralI32(0), LiteralI32(-1))))}))))));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Lt(y, maxY))));
  e.ok(fn.assign(
      nx, e(Add(nx, e(fn.callFunction("horizontalDifference",
                                      {e(Add(coord, Vec2i(e, LiteralI32(0), LiteralI32(1))))}))))));
  e.ok(fn.endIf());

  const IrExpr ny =
      e(fn.addVar("ny", IrType::F32(),
                  e(Mul(LiteralF32(2.0f), e(fn.callFunction("verticalDifference", {coord}))))));
  e.ok(fn.beginIf(e(Gt(x, minX))));
  e.ok(fn.assign(
      ny,
      e(Add(ny, e(fn.callFunction("verticalDifference",
                                  {e(Add(coord, Vec2i(e, LiteralI32(-1), LiteralI32(0))))}))))));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Lt(x, maxX))));
  e.ok(fn.assign(
      ny, e(Add(ny, e(fn.callFunction("verticalDifference",
                                      {e(Add(coord, Vec2i(e, LiteralI32(1), LiteralI32(0))))}))))));
  e.ok(fn.endIf());

  const IrExpr xBorder = e(Or(e(Eq(x, minX)), e(Eq(x, maxX))));
  const IrExpr yBorder = e(Or(e(Eq(y, minY)), e(Eq(y, maxY))));
  const IrExpr divisor =
      e(fn.addLet("divisor", e(CallBuiltin(BuiltinFn::Select, {LiteralF32(4.0f), LiteralF32(3.0f),
                                                               e(And(xBorder, yBorder))}))));
  const IrExpr scale = e(Neg(ParamsMember(e, fn, "surfaceScale")));
  const IrExpr normal = Vec3f(e, e(Div(e(Mul(scale, nx)), divisor)),
                              e(Div(e(Mul(scale, ny)), divisor)), LiteralF32(1.0f));
  e.ok(fn.returnValue(e(fn.callFunction("safeNormalize", {normal}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddComputeLightDirection(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> result = builder.createFunction(
      "computeLightDirection",
      {IrParam{"coord", IrType::Vec2i()}, IrParam{"surfaceZ", IrType::F32()}}, IrType::Vec3f());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr params = e(fn.ref("params"));
  e.ok(fn.beginIf(e(Eq(e(Member(params, "lightType")), LiteralU32(0)))));
  const IrExpr azimuth = e(Member(params, "azimuthRad"));
  const IrExpr elevation = e(Member(params, "elevationRad"));
  const IrExpr cosAzimuth = e(CallBuiltin(BuiltinFn::Cos, {azimuth}));
  const IrExpr sinAzimuth = e(CallBuiltin(BuiltinFn::Sin, {azimuth}));
  const IrExpr cosElevation = e(CallBuiltin(BuiltinFn::Cos, {elevation}));
  const IrExpr sinElevation = e(CallBuiltin(BuiltinFn::Sin, {elevation}));
  const IrExpr direction =
      Vec3f(e, e(Mul(cosAzimuth, cosElevation)), e(Mul(sinAzimuth, cosElevation)), sinElevation);
  e.ok(fn.returnValue(e(fn.callFunction("safeNormalize", {direction}))));
  e.ok(fn.endIf());

  const IrExpr coord = e(fn.ref("coord"));
  const IrExpr position =
      Vec3f(e, e(Convert(IrType::F32(), e(Swizzle(coord, "x")))),
            e(Convert(IrType::F32(), e(Swizzle(coord, "y")))), e(fn.ref("surfaceZ")));
  const IrExpr lightPosition = Vec3f(e, e(Member(params, "lightX")), e(Member(params, "lightY")),
                                     e(Member(params, "lightZ")));
  e.ok(fn.returnValue(e(fn.callFunction("safeNormalize", {e(Sub(lightPosition, position))}))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

ShaderStatus AddSpotLightFactor(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> result =
      builder.createFunction("spotLightFactor",
                             {IrParam{"coord", IrType::Vec2i()}, IrParam{"alpha", IrType::F32()},
                              IrParam{"lightDirection", IrType::Vec3f()}},
                             IrType::F32());
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr params = e(fn.ref("params"));
  e.ok(fn.beginIf(e(Ne(e(Member(params, "lightType")), LiteralU32(2)))));
  e.ok(fn.returnValue(LiteralF32(1.0f)));
  e.ok(fn.endIf());

  const IrExpr deviceSpotDirection = e(fn.callFunction(
      "safeNormalize",
      {Vec3f(e, e(Sub(e(Member(params, "pointsAtX")), e(Member(params, "lightX")))),
             e(Sub(e(Member(params, "pointsAtY")), e(Member(params, "lightY")))),
             e(Sub(e(Member(params, "pointsAtZ")), e(Member(params, "lightZ")))))}));
  const IrExpr cosAngleDevice = e(fn.addLet(
      "cosAngleDevice",
      e(CallBuiltin(BuiltinFn::Dot, {e(Neg(e(fn.ref("lightDirection")))), deviceSpotDirection}))));
  e.ok(fn.beginIf(e(Le(cosAngleDevice, LiteralF32(0.0f)))));
  e.ok(fn.returnValue(LiteralF32(0.0f)));
  e.ok(fn.endIf());

  const IrExpr cosAngle = e(fn.addVar("cosAngle", IrType::F32(), cosAngleDevice));
  e.ok(fn.beginIf(e(Ne(e(Member(params, "hasShear")), LiteralU32(0)))));
  const IrExpr coord = e(fn.ref("coord"));
  const IrExpr coordX = e(Convert(IrType::F32(), e(Swizzle(coord, "x"))));
  const IrExpr coordY = e(Convert(IrType::F32(), e(Swizzle(coord, "y"))));
  const IrExpr userX =
      e(fn.addLet("userX", e(Add(e(Add(e(Mul(e(Member(params, "pixelToUser0")), coordX)),
                                       e(Mul(e(Member(params, "pixelToUser1")), coordY)))),
                                 e(Member(params, "pixelToUser2"))))));
  const IrExpr userY =
      e(fn.addLet("userY", e(Add(e(Add(e(Mul(e(Member(params, "pixelToUser3")), coordX)),
                                       e(Mul(e(Member(params, "pixelToUser4")), coordY)))),
                                 e(Member(params, "pixelToUser5"))))));
  const IrExpr userZ = e(Mul(e(Member(params, "surfaceScale")), e(fn.ref("alpha"))));
  const IrExpr userLightToSurface =
      e(fn.callFunction("safeNormalize", {Vec3f(e, e(Sub(userX, e(Member(params, "userLightX")))),
                                                e(Sub(userY, e(Member(params, "userLightY")))),
                                                e(Sub(userZ, e(Member(params, "userLightZ")))))}));
  const IrExpr userSpotDirection = e(fn.callFunction(
      "safeNormalize",
      {Vec3f(e, e(Sub(e(Member(params, "userPointsAtX")), e(Member(params, "userLightX")))),
             e(Sub(e(Member(params, "userPointsAtY")), e(Member(params, "userLightY")))),
             e(Sub(e(Member(params, "userPointsAtZ")), e(Member(params, "userLightZ")))))}));
  e.ok(
      fn.assign(cosAngle, e(CallBuiltin(BuiltinFn::Dot, {userLightToSurface, userSpotDirection}))));
  e.ok(fn.beginIf(e(Le(cosAngle, LiteralF32(0.0f)))));
  e.ok(fn.returnValue(LiteralF32(0.0f)));
  e.ok(fn.endIf());
  e.ok(fn.endIf());

  const IrExpr coneFactor = e(fn.addVar("coneFactor", IrType::F32(), LiteralF32(1.0f)));
  e.ok(fn.beginIf(e(Ne(e(Member(params, "hasConeAngle")), LiteralU32(0)))));
  const IrExpr cosOuter =
      e(fn.addLet("cosOuter", e(CallBuiltin(BuiltinFn::Cos, {e(Member(params, "coneAngleRad"))}))));
  const IrExpr cosInner = e(fn.addLet("cosInner", e(Add(cosOuter, LiteralF32(0.016f)))));
  e.ok(fn.beginIf(e(Lt(cosAngle, cosOuter))));
  e.ok(fn.returnValue(LiteralF32(0.0f)));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Lt(cosAngle, cosInner))));
  e.ok(fn.assign(coneFactor, e(Div(e(Sub(cosAngle, cosOuter)), LiteralF32(0.016f)))));
  e.ok(fn.endIf());
  e.ok(fn.endIf());

  const IrExpr exponent = e(fn.addLet(
      "exponent", e(CallBuiltin(BuiltinFn::Select,
                                {LiteralF32(1.0f), e(Member(params, "spotExponent")),
                                 e(Gt(e(Member(params, "spotExponent")), LiteralF32(0.0f)))}))));
  e.ok(fn.returnValue(e(Mul(e(CallBuiltin(BuiltinFn::Pow, {cosAngle, exponent})), coneFactor))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

IrType LightingParamsType(ErrorLatch& e) {
  const IrType f32 = IrType::F32();
  return e(IrType::Struct("LightingParams", {IrType::Member{"surfaceScale", f32},
                                             IrType::Member{"lightingConstant", f32},
                                             IrType::Member{"specularExponent", f32},
                                             IrType::Member{"pad0", f32},
                                             IrType::Member{"lightR", f32},
                                             IrType::Member{"lightG", f32},
                                             IrType::Member{"lightB", f32},
                                             IrType::Member{"lightType", IrType::U32()},
                                             IrType::Member{"azimuthRad", f32},
                                             IrType::Member{"elevationRad", f32},
                                             IrType::Member{"lightX", f32},
                                             IrType::Member{"lightY", f32},
                                             IrType::Member{"lightZ", f32},
                                             IrType::Member{"userLightX", f32},
                                             IrType::Member{"userLightY", f32},
                                             IrType::Member{"userLightZ", f32},
                                             IrType::Member{"pointsAtX", f32},
                                             IrType::Member{"pointsAtY", f32},
                                             IrType::Member{"pointsAtZ", f32},
                                             IrType::Member{"spotExponent", f32},
                                             IrType::Member{"userPointsAtX", f32},
                                             IrType::Member{"userPointsAtY", f32},
                                             IrType::Member{"userPointsAtZ", f32},
                                             IrType::Member{"coneAngleRad", f32},
                                             IrType::Member{"pixelToUser0", f32},
                                             IrType::Member{"pixelToUser1", f32},
                                             IrType::Member{"pixelToUser2", f32},
                                             IrType::Member{"pixelToUser3", f32},
                                             IrType::Member{"pixelToUser4", f32},
                                             IrType::Member{"pixelToUser5", f32},
                                             IrType::Member{"hasShear", IrType::U32()},
                                             IrType::Member{"hasConeAngle", IrType::U32()},
                                             IrType::Member{"sampleMinX", IrType::I32()},
                                             IrType::Member{"sampleMinY", IrType::I32()},
                                             IrType::Member{"sampleMaxX", IrType::I32()},
                                             IrType::Member{"sampleMaxY", IrType::I32()}}));
}

ShaderResult<IrModule> BuildLightingModule(LightingModel model) {
  ErrorLatch e;
  ModuleBuilder builder;
  const auto binding = [](LightingBinding value) { return static_cast<uint32_t>(value); };
  const IrType paramsType = LightingParamsType(e);
  e.ok(builder.addTexture2d(0, binding(LightingBinding::InputTexture), "inputTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, binding(LightingBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba32Float));
  e.ok(builder.addReadOnlyStorageBuffer(0, binding(LightingBinding::Params), "params", paramsType));
  e.ok(AddSafeNormalize(builder));
  e.ok(AddHeightAt(builder));
  e.ok(AddHorizontalDifference(builder));
  e.ok(AddVerticalDifference(builder));
  e.ok(AddComputeNormal(builder));
  e.ok(AddComputeLightDirection(builder));
  e.ok(AddSpotLightFactor(builder));
  if (model == LightingModel::Specular) {
    e.ok(AddSvgPow(builder));
  }

  ShaderResult<FunctionBuilder> result = builder.createComputeEntryPoint(
      RcString(kLightingEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kLightingWorkgroupSize, kLightingWorkgroupSize, 1});
  if (result.hasError()) {
    return std::move(result).error();
  }
  FunctionBuilder fn = std::move(result).result();
  const IrExpr gid = e(fn.ref("gid"));
  const IrExpr output = e(fn.ref("outputTexture"));
  const IrExpr extent =
      e(fn.addLet("extent", e(CallBuiltin(BuiltinFn::TextureDimensions, {output}))));
  e.ok(fn.beginIf(e(Or(e(Ge(e(Swizzle(gid, "x")), e(Swizzle(extent, "x")))),
                       e(Ge(e(Swizzle(gid, "y")), e(Swizzle(extent, "y"))))))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr coord = e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const IrExpr params = e(fn.ref("params"));
  const IrExpr outside =
      e(Or(e(Or(e(Lt(e(Swizzle(coord, "x")), e(Member(params, "sampleMinX")))),
                e(Gt(e(Swizzle(coord, "x")), e(Member(params, "sampleMaxX")))))),
           e(Or(e(Lt(e(Swizzle(coord, "y")), e(Member(params, "sampleMinY")))),
                e(Gt(e(Swizzle(coord, "y")), e(Member(params, "sampleMaxY"))))))));
  e.ok(fn.beginIf(outside));
  e.ok(fn.textureStore(
      output, coord,
      Vec4f(e, LiteralF32(0.0f), LiteralF32(0.0f), LiteralF32(0.0f), LiteralF32(0.0f))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr normal = e(fn.addLet("normal", e(fn.callFunction("computeNormal", {coord}))));
  const IrExpr alpha = e(fn.addLet("alpha", e(fn.callFunction("heightAt", {coord}))));
  const IrExpr surfaceZ =
      e(fn.addLet("surfaceZ", e(Mul(e(Member(params, "surfaceScale")), alpha))));
  const IrExpr lightDirection = e(
      fn.addLet("lightDirection", e(fn.callFunction("computeLightDirection", {coord, surfaceZ}))));
  const IrExpr spot =
      e(fn.addLet("spot", e(fn.callFunction("spotLightFactor", {coord, alpha, lightDirection}))));

  IrExpr response = LiteralF32(0.0f);
  if (model == LightingModel::Diffuse) {
    response =
        e(CallBuiltin(BuiltinFn::Max, {e(CallBuiltin(BuiltinFn::Dot, {normal, lightDirection})),
                                       LiteralF32(0.0f)}));
  } else {
    const IrExpr eye = Vec3f(e, LiteralF32(0.0f), LiteralF32(0.0f), LiteralF32(1.0f));
    const IrExpr halfway =
        e(fn.addLet("halfway", e(fn.callFunction("safeNormalize", {e(Add(lightDirection, eye))}))));
    const IrExpr normalDotHalf = e(CallBuiltin(
        BuiltinFn::Max, {e(CallBuiltin(BuiltinFn::Dot, {normal, halfway})), LiteralF32(0.0f)}));
    response = e(fn.callFunction("svgPow", {normalDotHalf, e(Member(params, "specularExponent"))}));
  }
  const IrExpr intensity = e(fn.addLet(
      "intensity", e(Mul(e(Mul(e(Member(params, "lightingConstant")), response)), spot))));
  const auto litChannel = [&](const char* member) {
    return e(CallBuiltin(BuiltinFn::Clamp, {e(Mul(intensity, e(Member(params, member)))),
                                            LiteralF32(0.0f), LiteralF32(1.0f)}));
  };
  const IrExpr red = e(fn.addLet("red", litChannel("lightR")));
  const IrExpr green = e(fn.addLet("green", litChannel("lightG")));
  const IrExpr blue = e(fn.addLet("blue", litChannel("lightB")));
  IrExpr outputAlpha = LiteralF32(1.0f);
  if (model == LightingModel::Specular) {
    outputAlpha =
        e(CallBuiltin(BuiltinFn::Max, {red, e(CallBuiltin(BuiltinFn::Max, {green, blue}))}));
  }
  e.ok(fn.textureStore(output, coord, Vec4f(e, red, green, blue, outputAlpha)));
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace

ShaderResult<IrModule> BuildDiffuseLightingModule() {
  return BuildLightingModule(LightingModel::Diffuse);
}

ShaderResult<IrModule> BuildSpecularLightingModule() {
  return BuildLightingModule(LightingModel::Specular);
}

}  // namespace donner::gpu::shader::programs
