#include "donner/gpu/shader/programs/SnapshotUnpremultiply.h"

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

  /// Latches a status. @param status Status to check.
  void ok(ShaderStatus&& status) {
    if (status.hasError() && !error) {
      error = std::move(status).error();
    }
  }
};

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(SnapshotUnpremultiplyBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// The stored 8-bit channel behind a normalized unorm texel component.
/// @param e Error latch. @param texel Loaded `vec4<f32>` texel.
/// @param component Swizzle naming the component, e.g. "x".
IrExpr Channel8(Latch& e, const IrExpr& texel, const char* component) {
  return e(Convert(IrType::U32(),
                   e(CallBuiltin(BuiltinFn::Round,
                                 {e(Mul(e(Swizzle(texel, component)), LiteralF32(255.0f)))}))));
}

/// One straight-alpha channel, as the CPU reference computes it:
/// `min(255, (premul * 255 + alpha / 2) / alpha)`, evaluated only where alpha is nonzero.
/// @param e Error latch. @param premul8 Premultiplied 8-bit channel.
/// @param alpha8 8-bit alpha. @param halfAlpha `alpha8 / 2`, the round-half-up addend.
IrExpr StraightChannel(Latch& e, const IrExpr& premul8, const IrExpr& alpha8,
                       const IrExpr& halfAlpha) {
  return e(CallBuiltin(
      BuiltinFn::Min,
      {LiteralU32(255), e(Div(e(Add(e(Mul(premul8, LiteralU32(255))), halfAlpha)), alpha8))}));
}

/// The normalized float a stored 8-bit channel becomes on unorm storage.
/// @param e Error latch. @param channel8 8-bit channel.
IrExpr Normalized(Latch& e, const IrExpr& channel8) {
  return e(Div(e(Convert(IrType::F32(), channel8)), LiteralF32(255.0f)));
}

}  // namespace

ShaderResult<IrModule> BuildSnapshotUnpremultiplyModule() {
  Latch e;
  ModuleBuilder builder;

  e.ok(builder.addTexture2d(0, BindingIndex(SnapshotUnpremultiplyBinding::InputTexture),
                            "inputTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(
      0, BindingIndex(SnapshotUnpremultiplyBinding::OutputTexture), "outputTexture",
      StorageTextureFormat::Rgba8Unorm));

  auto entryResult = builder.createComputeEntryPoint(
      "cs_main",
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kSnapshotUnpremultiplyWorkgroupSize, kSnapshotUnpremultiplyWorkgroupSize, 1});
  if (entryResult.hasError()) {
    return std::move(entryResult).error();
  }
  FunctionBuilder fn = std::move(entryResult).result();

  const IrExpr gid = e(fn.ref("gid"));
  const IrExpr inputTexture = e(fn.ref("inputTexture"));
  const IrExpr outputTexture = e(fn.ref("outputTexture"));

  // Clamped to BOTH textures. The staging output is pooled, so writing only the region the input
  // covers would silently leave a previous snapshot's pixels in the rest of a larger pooled
  // texture rather than raising a validation error.
  const IrExpr extent = e(fn.addLet(
      "extent", e(CallBuiltin(BuiltinFn::Min,
                              {e(CallBuiltin(BuiltinFn::TextureDimensions, {inputTexture})),
                               e(CallBuiltin(BuiltinFn::TextureDimensions, {outputTexture}))}))));
  e.ok(fn.beginIf(e(CallBuiltin(BuiltinFn::Any, {e(Ge(e(Swizzle(gid, "xy")), extent))}))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr coords = e(fn.addLet("coords", e(Convert(IrType::Vec2i(), e(Swizzle(gid, "xy"))))));
  const IrExpr premul = e(fn.addLet(
      "premul", e(CallBuiltin(BuiltinFn::TextureLoad, {inputTexture, coords, LiteralI32(0)}))));

  const IrExpr r8 = e(fn.addLet("r8", Channel8(e, premul, "x")));
  const IrExpr g8 = e(fn.addLet("g8", Channel8(e, premul, "y")));
  const IrExpr b8 = e(fn.addLet("b8", Channel8(e, premul, "z")));
  const IrExpr a8 = e(fn.addLet("a8", Channel8(e, premul, "w")));

  // Fully transparent texels stay (0, 0, 0, 0): there is no color to recover, and dividing by
  // zero alpha is exactly the case the CPU reference special-cases.
  const IrExpr sr = e(fn.addVar("sr", IrType::U32(), LiteralU32(0)));
  const IrExpr sg = e(fn.addVar("sg", IrType::U32(), LiteralU32(0)));
  const IrExpr sb = e(fn.addVar("sb", IrType::U32(), LiteralU32(0)));

  e.ok(fn.beginIf(e(Ne(a8, LiteralU32(0)))));
  const IrExpr halfAlpha = e(fn.addLet("halfAlpha", e(Shr(a8, LiteralU32(1)))));
  e.ok(fn.assign(sr, StraightChannel(e, r8, a8, halfAlpha)));
  e.ok(fn.assign(sg, StraightChannel(e, g8, a8, halfAlpha)));
  e.ok(fn.assign(sb, StraightChannel(e, b8, a8, halfAlpha)));
  e.ok(fn.endIf());

  const IrExpr straight = e(fn.addLet(
      "straight", e(ConstructVector(IrType::Vec4f(), {Normalized(e, sr), Normalized(e, sg),
                                                      Normalized(e, sb), Normalized(e, a8)}))));
  e.ok(fn.textureStore(outputTexture, coords, straight));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
