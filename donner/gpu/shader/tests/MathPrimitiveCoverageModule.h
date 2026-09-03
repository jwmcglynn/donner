#pragma once
/// @file
/// A compute module exercising `sign`, `floor`, and `pow` in both their scalar and vector forms,
/// shared by the emitter goldens and by the platform validators.
///
/// The three opcodes reach no shipping program yet, so without this module their spellings and
/// their SPIR-V lowerings would be checked only against the emitters' own idea of them. It also
/// carries the composition the filter primitives need, `sign(x) * floor(abs(x) + 0.5)`: that
/// recipe is round-half-away-from-zero, which is what the CPU filter path does and what WGSL
/// `round` does not. The recipe itself lives in the programs library, so this module and the
/// shipping program that offsets pixels declare one function rather than two.
///
/// Each invocation reads one f32 from the input buffer and writes one texel encoding four
/// results, so the same module doubles as an executable check of those results against the host.

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"
#include "donner/gpu/shader/programs/RoundHalfAwayFromZero.h"

namespace donner::gpu::shader {

/// Invocations per workgroup along x; the module's only dispatch axis.
inline constexpr uint32_t kMathPrimitiveWorkgroupSize = 8;

/// Encoded value added to a signed result before it is divided by 255, so the whole signed range
/// the tests use lands in an exactly representable rgba8unorm code point.
inline constexpr float kMathPrimitiveSignedBias = 128.0f;

/// Multiplier the sign result is encoded with: -1, 0, and 1 become the byte values 0, 100, and
/// 200, which no rounding rule can push into a neighbour.
inline constexpr float kMathPrimitiveSignScale = 100.0f;

/// The values every check of these primitives runs over: exact halves in both signs, where
/// round-half-away-from-zero and WGSL's round-half-to-even disagree; values just under and just
/// over a half; both zeros; and whole numbers. The count is a whole number of workgroups, so the
/// dispatch needs no bounds guard and every lane writes a texel.
inline std::vector<float> MathPrimitiveInputValues() {
  return {0.0f, -0.0f, 0.5f,   -0.5f,   1.5f,    -1.5f,    2.5f,    -2.5f,
          3.5f, -3.5f, 12.5f,  -12.5f,  0.4999f, -0.4999f, 0.5001f, -0.5001f,
          7.0f, -7.0f, 100.5f, -100.5f, 3.49f,   -3.49f,   42.0f,   -42.0f};
}

/// The module's rounding recipe evaluated on the host, in the same order and the same precision,
/// so a divergence between it and `std::round` is a statement about the recipe rather than about
/// two different formulas.
///
/// @param value Value to round.
inline float RoundHalfAwayFromZeroOnHost(float value) {
  const float sign = (value > 0.0f) ? 1.0f : ((value < 0.0f) ? -1.0f : 0.0f);
  return sign * std::floor(std::abs(value) + 0.5f);
}

/// Builds the compute module. Binding 0 is the destination storage texture, one texel per input;
/// binding 1 is the read-only f32 input buffer.
inline ShaderResult<IrModule> BuildMathPrimitiveModule() {
  programs::ErrorLatch e;
  ModuleBuilder builder;

  e.ok(builder.addWriteOnlyStorageTexture2d(0, 0, "outputTexture",
                                            StorageTextureFormat::Rgba8Unorm));
  const IrType valuesType = e(IrType::RuntimeArray(IrType::F32()));
  e.ok(builder.addReadOnlyStorageBuffer(0, 1, "values", valuesType));

  e.ok(programs::AddRoundHalfAwayFromZero(builder));

  ShaderResult<FunctionBuilder> entry =
      builder.createComputeEntryPoint("cs_main",
                                      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
                                               BuiltinInput::GlobalInvocationId}},
                                      WorkgroupSize{kMathPrimitiveWorkgroupSize, 1, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  FunctionBuilder fn = std::move(entry).result();

  const IrExpr gid = e(fn.ref("gid"));
  const IrExpr outputTexture = e(fn.ref("outputTexture"));
  const IrExpr lane = e(Swizzle(gid, "x"));
  const IrExpr x = e(fn.addLet("x", e(Index(e(fn.ref("values")), lane))));

  const IrExpr rounded = e(fn.addLet(
      "rounded", e(fn.callFunction(RcString(programs::kRoundHalfAwayFromZeroName), {x}))));

  // A quarter of the input, and its negation, so the vector forms of sign and floor run on two
  // components that disagree in sign rather than on a splat that would hide a swapped component.
  const IrExpr axes =
      e(fn.addLet("axes", e(ConstructVector(IrType::Vec2f(), {e(Mul(x, LiteralF32(0.25f))),
                                                              e(Mul(x, LiteralF32(-0.25f)))}))));
  const IrExpr axisSigns = e(fn.addLet("axisSigns", e(CallBuiltin(BuiltinFn::Sign, {axes}))));
  const IrExpr axisFloors = e(fn.addLet("axisFloors", e(CallBuiltin(BuiltinFn::Floor, {axes}))));

  // pow is undefined for a negative base, so the base is clamped into [0, 1] before it is used.
  const IrExpr straight = e(fn.addLet("straight", e(CallBuiltin(BuiltinFn::Saturate, {x}))));
  const IrExpr linearized = e(fn.addLet(
      "linearized", e(CallBuiltin(BuiltinFn::Pow,
                                  {e(Div(e(Add(straight, LiteralF32(0.055f))), LiteralF32(1.055f))),
                                   LiteralF32(2.4f)}))));
  const IrExpr curved = e(fn.addLet(
      "curved",
      e(CallBuiltin(
          BuiltinFn::Pow,
          {e(ConstructVector(IrType::Vec2f(), {straight, e(Sub(LiteralF32(1.0f), straight))})),
           e(ConstructVector(IrType::Vec2f(), {LiteralF32(2.4f), LiteralF32(2.4f)}))}))));

  const IrExpr encodedRound =
      e(Div(e(Add(rounded, LiteralF32(kMathPrimitiveSignedBias))), LiteralF32(255.0f)));
  const IrExpr encodedSign =
      e(Div(e(Add(e(Mul(e(Swizzle(axisSigns, "x")), LiteralF32(kMathPrimitiveSignScale))),
                  LiteralF32(kMathPrimitiveSignScale))),
            LiteralF32(255.0f)));
  const IrExpr encodedFloor =
      e(Div(e(Add(e(Swizzle(axisFloors, "x")), LiteralF32(kMathPrimitiveSignedBias))),
            LiteralF32(255.0f)));
  const IrExpr encodedPow =
      e(Mul(LiteralF32(0.25f),
            e(Add(e(Add(linearized, e(Swizzle(curved, "x")))), e(Swizzle(curved, "y"))))));

  e.ok(fn.textureStore(
      outputTexture,
      e(ConstructVector(IrType::Vec2i(), {e(Convert(IrType::I32(), lane)), LiteralI32(0)})),
      e(ConstructVector(IrType::Vec4f(), {encodedRound, encodedSign, encodedFloor, encodedPow}))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader
