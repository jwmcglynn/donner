#include "donner/gpu/shader/programs/ColorMatrix.h"

#include <optional>
#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"

namespace donner::gpu::shader::programs {

namespace {

/**
 * Latches the first builder error so the program can be transliterated linearly. On error every
 * subsequent expression receives a dummy `0.0f`; the resulting cascade errors are ignored because
 * only the first is reported. The inputs are static, so any latched error is a Donner bug
 * surfaced by the golden test, never a runtime condition.
 */
struct Latch {
  std::optional<ShaderError> error;  //!< First error, if any.

  /// Unwraps an expression result. @param result Result to unwrap.
  IrExpr operator()(ShaderResult<IrExpr>&& result) {
    if (result.hasError()) {
      if (!error) {
        error = std::move(result).error();
      }
      return LiteralF32(0.0f);
    }
    return std::move(result).result();
  }

  /// Unwraps a type result. @param result Result to unwrap.
  IrType operator()(ShaderResult<IrType>&& result) {
    if (result.hasError()) {
      if (!error) {
        error = std::move(result).error();
      }
      return IrType::F32();
    }
    return std::move(result).result();
  }

  /// Latches a status. @param status Status to check.
  void ok(ShaderStatus&& status) {
    if (status.hasError() && !error) {
      error = std::move(status).error();
    }
  }
};

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(ColorMatrixBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Adds the four module-scope bindings the entry point reads and writes.
ShaderStatus AddBindings(ModuleBuilder& builder, const IrType& paramsType,
                         const IrType& biasArrayType) {
  if (ShaderStatus status =
          builder.addTexture2d(0, BindingIndex(ColorMatrixBinding::InputTexture), "inputTexture");
      status.hasError()) {
    return status;
  }
  if (ShaderStatus status =
          builder.addWriteOnlyStorageTexture2d(0, BindingIndex(ColorMatrixBinding::OutputTexture),
                                               "outputTexture", StorageTextureFormat::Rgba8Unorm);
      status.hasError()) {
    return status;
  }
  if (ShaderStatus status = builder.addUniformBuffer(0, BindingIndex(ColorMatrixBinding::Params),
                                                     "params", paramsType);
      status.hasError()) {
    return status;
  }
  return builder.addReadOnlyStorageBuffer(0, BindingIndex(ColorMatrixBinding::Bias), "bias",
                                          biasArrayType);
}

}  // namespace

ShaderResult<IrModule> BuildColorMatrixModule() {
  Latch e;
  ModuleBuilder builder;

  const IrType vec4f = IrType::Vec4f();
  const IrType paramsType = e(IrType::Struct(
      "ColorMatrixParams",
      {IrType::Member{"row0", vec4f}, IrType::Member{"row1", vec4f}, IrType::Member{"row2", vec4f},
       IrType::Member{"row3", vec4f}, IrType::Member{"row4", vec4f}}));
  const IrType biasArrayType = e(IrType::RuntimeArray(vec4f));
  e.ok(AddBindings(builder, paramsType, biasArrayType));

  auto entryResult = builder.createComputeEntryPoint(
      "cs_main",
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kColorMatrixWorkgroupSize, kColorMatrixWorkgroupSize, 1});
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

  // A checkerboard bias index keeps the storage-buffer read dynamic rather than a constant fold.
  const IrExpr biasIndex = e(fn.addLet(
      "biasIndex",
      e(CallBuiltin(BuiltinFn::Select, {LiteralU32(0), LiteralU32(1),
                                        e(Gt(e(Swizzle(gid, "x")), e(Swizzle(gid, "y"))))}))));
  const IrExpr biasValue = e(fn.addLet("biasValue", e(Index(e(fn.ref("bias")), biasIndex))));

  const IrExpr params = e(fn.ref("params"));
  const IrExpr weighted = e(fn.addLet(
      "weighted", e(Add(e(Add(e(Add(e(Mul(e(Member(params, "row0")), e(Swizzle(source, "x")))),
                                    e(Mul(e(Member(params, "row1")), e(Swizzle(source, "y")))))),
                              e(Mul(e(Member(params, "row2")), e(Swizzle(source, "z")))))),
                        e(Mul(e(Member(params, "row3")), e(Swizzle(source, "w"))))))));
  // The fifth column is a constant added after the weighted sum, not a multiplier: an SVG color
  // matrix is four rows of five, where the last column shifts each output channel regardless of
  // the input. A caller with nothing to shift passes zeros, which leaves the result unchanged.
  const IrExpr shifted = e(fn.addLet("shifted", e(Add(weighted, e(Member(params, "row4"))))));
  const IrExpr result =
      e(fn.addLet("result", e(CallBuiltin(BuiltinFn::Saturate, {e(Add(shifted, biasValue))}))));

  e.ok(fn.textureStore(outputTexture, coords, result));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
