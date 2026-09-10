#include "donner/gpu/shader/programs/ColorSpaceConvert.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {

namespace {

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(ColorSpaceConvertBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Adds the four module-scope bindings the entry point reads and writes.
ShaderStatus AddBindings(ModuleBuilder& builder, const IrType& paramsType) {
  if (ShaderStatus status = builder.addTexture2d(
          0, BindingIndex(ColorSpaceConvertBinding::InputTexture), "inputTexture");
      status.hasError()) {
    return status;
  }
  if (ShaderStatus status = builder.addWriteOnlyStorageTexture2d(
          0, BindingIndex(ColorSpaceConvertBinding::OutputTexture), "outputTexture",
          StorageTextureFormat::Rgba32Float);
      status.hasError()) {
    return status;
  }
  if (ShaderStatus status = builder.addUniformBuffer(
          0, BindingIndex(ColorSpaceConvertBinding::Params), "params", paramsType);
      status.hasError()) {
    return status;
  }
  return AddColorTransferFunctions(builder, BindingIndex(ColorSpaceConvertBinding::TransferTable));
}

/// Declares one transfer direction using bounded nearest-sample indexing.
/// @param builder Destination module. @param name Function name. @param offset Table half.
ShaderStatus AddTransferFunction(ModuleBuilder& builder, const char* name, uint32_t offset) {
  ErrorLatch e;
  auto created = builder.createFunction(name, {IrParam{"c", IrType::F32()}}, IrType::F32());
  if (created.hasError()) {
    return std::move(created).error();
  }
  FunctionBuilder fn = std::move(created).result();
  const IrExpr c = e(fn.ref("c"));
  const IrExpr unit = e(fn.addVar("unit", IrType::F32(), LiteralF32(0.0f)));
  // The comparison maps NaN to zero before conversion to an integer index.
  e.ok(fn.beginIf(e(Gt(c, LiteralF32(0.0f)))));
  e.ok(fn.assign(unit, e(CallBuiltin(BuiltinFn::Min, {c, LiteralF32(1.0f)}))));
  e.ok(fn.endIf());
  const IrExpr scaled = e(fn.addLet(
      "scaled", e(Mul(unit, LiteralF32(static_cast<float>(kColorTransferSampleCount - 1))))));
  const IrExpr index =
      e(fn.addLet("index", e(Convert(IrType::U32(), e(Add(scaled, LiteralF32(0.5f)))))));
  e.ok(fn.returnValue(e(
      Index(e(Member(e(fn.ref("transferTable")), "samples")), e(Add(index, LiteralU32(offset)))))));
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
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

ShaderStatus AddColorTransferFunctions(ModuleBuilder& builder, uint32_t binding) {
  auto tableType = IrType::SizedArray(IrType::F32(), 2 * kColorTransferSampleCount);
  if (tableType.hasError()) {
    return tableType.error();
  }
  auto blockType = IrType::Struct("ColorTransferTable", {{"samples", tableType.result()}});
  if (blockType.hasError()) {
    return blockType.error();
  }
  const ShaderStatus status =
      builder.addReadOnlyStorageBuffer(0, binding, "transferTable", blockType.result());
  if (status.hasError()) {
    return status;
  }
  if (ShaderStatus transfer = AddTransferFunction(builder, "srgb_channel_to_linear", 0);
      transfer.hasError()) {
    return transfer;
  }
  return AddTransferFunction(builder, "linear_channel_to_srgb", kColorTransferSampleCount);
}

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
  e.ok(fn.assign(
      straight, e(ConstructVector(vec4f, {e(Mul(e(Swizzle(source, "xyz")),
                                                e(Div(LiteralF32(1.0f), e(Swizzle(source, "w")))))),
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
