#include "donner/gpu/shader/programs/ColorSpaceConvert.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {

namespace {

/// Channel value at or below which the sRGB transfer is its linear segment rather than its curve,
/// expressed in the sRGB-encoded space the segment is read in.
constexpr float kSrgbLinearSegmentEnd = 0.04045f;

/// The same breakpoint expressed in linear light, which is where the inverse transfer reads it.
constexpr float kLinearSegmentEnd = 0.0031308f;

/// Slope of the linear segment, shared by the transfer and its inverse.
constexpr float kLinearSegmentSlope = 12.92f;

/// Offset the power curve is shifted by, so the two segments meet.
constexpr float kCurveOffset = 0.055f;

/// Scale the power curve is divided by on the way to linear and multiplied by on the way back.
constexpr float kCurveScale = 1.055f;

/// Exponent of the curve, from sRGB encoding to linear light.
constexpr float kCurveExponent = 2.4f;

/// Exponent of the curve, from linear light back to sRGB encoding.
constexpr float kInverseCurveExponent = 1.0f / kCurveExponent;

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(ColorSpaceConvertBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Adds the three module-scope bindings the entry point reads and writes.
ShaderStatus AddBindings(ModuleBuilder& builder, const IrType& paramsType) {
  if (ShaderStatus status = builder.addTexture2d(
          0, BindingIndex(ColorSpaceConvertBinding::InputTexture), "inputTexture");
      status.hasError()) {
    return status;
  }
  if (ShaderStatus status = builder.addWriteOnlyStorageTexture2d(
          0, BindingIndex(ColorSpaceConvertBinding::OutputTexture), "outputTexture",
          StorageTextureFormat::Rgba8Unorm);
      status.hasError()) {
    return status;
  }
  return builder.addUniformBuffer(0, BindingIndex(ColorSpaceConvertBinding::Params), "params",
                                  paramsType);
}

/// Declares `srgb_channel_to_linear(c: f32) -> f32`, the sRGB transfer's forward direction.
/// @param builder Module to declare the function on.
ShaderStatus AddSrgbChannelToLinear(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> created = builder.createFunction(
      "srgb_channel_to_linear", {IrParam{"c", IrType::F32()}}, IrType::F32());
  if (created.hasError()) {
    return std::move(created).error();
  }
  FunctionBuilder fn = std::move(created).result();

  const IrExpr c = e(fn.ref("c"));
  e.ok(fn.beginIf(e(Le(c, LiteralF32(kSrgbLinearSegmentEnd)))));
  e.ok(fn.returnValue(e(Div(c, LiteralF32(kLinearSegmentSlope)))));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(e(CallBuiltin(
      BuiltinFn::Pow, {e(Div(e(Add(c, LiteralF32(kCurveOffset))), LiteralF32(kCurveScale))),
                       LiteralF32(kCurveExponent)}))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return OkShaderStatus();
}

/// Declares `linear_channel_to_srgb(c: f32) -> f32`, the sRGB transfer's inverse.
/// @param builder Module to declare the function on.
ShaderStatus AddLinearChannelToSrgb(ModuleBuilder& builder) {
  ErrorLatch e;
  ShaderResult<FunctionBuilder> created = builder.createFunction(
      "linear_channel_to_srgb", {IrParam{"c", IrType::F32()}}, IrType::F32());
  if (created.hasError()) {
    return std::move(created).error();
  }
  FunctionBuilder fn = std::move(created).result();

  const IrExpr c = e(fn.ref("c"));
  e.ok(fn.beginIf(e(Le(c, LiteralF32(kLinearSegmentEnd)))));
  e.ok(fn.returnValue(e(Mul(c, LiteralF32(kLinearSegmentSlope)))));
  e.ok(fn.endIf());
  e.ok(fn.returnValue(
      e(Sub(e(Mul(LiteralF32(kCurveScale),
                  e(CallBuiltin(BuiltinFn::Pow, {c, LiteralF32(kInverseCurveExponent)})))),
            LiteralF32(kCurveOffset)))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return OkShaderStatus();
}

/// `vec3<f32>(fn(v.x), fn(v.y), fn(v.z))`: a per-channel transfer applied to a color.
/// @param e Error latch. @param fn Function being built. @param name Transfer to call.
/// @param color Straight-alpha color to convert.
IrExpr ConvertedChannels(ErrorLatch& e, FunctionBuilder& fn, const RcString& name,
                         const IrExpr& color) {
  return e(ConstructVector(IrType::Vec3f(), {e(fn.callFunction(name, {e(Swizzle(color, "x"))})),
                                             e(fn.callFunction(name, {e(Swizzle(color, "y"))})),
                                             e(fn.callFunction(name, {e(Swizzle(color, "z"))}))}));
}

}  // namespace

ShaderResult<IrModule> BuildColorSpaceConvertModule() {
  ErrorLatch e;
  ModuleBuilder builder;

  const IrType u32 = IrType::U32();
  const IrType paramsType = e(IrType::Struct(
      "ColorSpaceConvertParams",
      {IrType::Member{"direction", u32},
       // One u32 member sizes the struct at 4 bytes. The trailing words carry that to 16, which
       // is the size a host mirror declared with 16-byte alignment computes for the same member.
       IrType::Member{"pad0", u32}, IrType::Member{"pad1", u32}, IrType::Member{"pad2", u32}}));
  e.ok(AddBindings(builder, paramsType));
  e.ok(AddSrgbChannelToLinear(builder));
  e.ok(AddLinearChannelToSrgb(builder));

  auto entryResult = builder.createComputeEntryPoint(
      RcString(kColorSpaceConvertEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kColorSpaceConvertWorkgroupSize, kColorSpaceConvertWorkgroupSize, 1});
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
  const IrExpr source = e(fn.addLet(
      "source",
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("inputTexture")), coords, LiteralI32(0)}))));

  const IrType vec4f = IrType::Vec4f();
  const IrExpr transparentBlack = e(ConstructVector(vec4f, {LiteralF32(0.0f)}));

  // The transfer is defined on straight-alpha values while the chain carries premultiplied ones.
  // A fully transparent texel has no straight-alpha color to recover and stays transparent black,
  // which is what the transfer of a zero channel produces anyway.
  const IrExpr straight = e(fn.addVar("straight", vec4f, transparentBlack));
  e.ok(fn.beginIf(e(Gt(e(Swizzle(source, "w")), LiteralF32(0.0f)))));
  e.ok(fn.assign(straight, e(ConstructVector(
                               vec4f, {e(Div(e(Swizzle(source, "xyz")), e(Swizzle(source, "w")))),
                                       e(Swizzle(source, "w"))}))));
  e.ok(fn.endIf());

  const IrExpr color = e(
      fn.addVar("color", IrType::Vec3f(), e(ConstructVector(IrType::Vec3f(), {LiteralF32(0.0f)}))));
  e.ok(fn.beginIf(e(Eq(e(Member(e(fn.ref("params")), "direction")),
                       LiteralU32(kColorSpaceConvertSrgbToLinear)))));
  e.ok(fn.assign(color, ConvertedChannels(e, fn, "srgb_channel_to_linear", straight)));
  e.ok(fn.elseBranch());
  e.ok(fn.assign(color, ConvertedChannels(e, fn, "linear_channel_to_srgb", straight)));
  e.ok(fn.endIf());

  // Saturate is the clamp to the representable range, and it is also what keeps the re-associated
  // color no greater than the alpha it was multiplied by.
  const IrExpr result =
      e(fn.addLet("result", e(ConstructVector(vec4f, {e(Mul(color, e(Swizzle(straight, "w")))),
                                                      e(Swizzle(straight, "w"))}))));
  e.ok(fn.textureStore(outputTexture, coords, e(CallBuiltin(BuiltinFn::Saturate, {result}))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
