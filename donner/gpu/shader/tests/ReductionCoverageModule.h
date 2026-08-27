#pragma once
/// @file
/// A tiny module exercising both bool-vector reductions, shared by the emitter tests and by the
/// platform validators.
///
/// It exists because `any` and `all` are the two IR builtins whose emitted form no shipping
/// program uses in both directions: the snapshot-unpremultiply kernel reduces with `any` only,
/// so `all` would otherwise reach neither a golden nor a real toolchain, and a mistake in its
/// branch, its spelling, or its operand order would ship silently.

#include <utility>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/IrModule.h"
#include "donner/gpu/shader/tests/ShaderTestUtils.h"

namespace donner::gpu::shader {

/// Builds a compute module whose only work is to reduce two componentwise comparisons, one with
/// `all` and one with `any`, and store a texel where the two agree.
///
/// The two reductions are deliberately complementary - `all(v < extent)` and
/// `any(v >= extent)` are negations of each other - so an emitter that confused them, or that
/// reduced the wrong operand, produces a store that never runs rather than one that looks right.
inline ShaderResult<IrModule> BuildVectorReductionModule() {
  ModuleBuilder builder;
  if (ShaderStatus status = builder.addWriteOnlyStorageTexture2d(0, 0, "outputTexture",
                                                                 StorageTextureFormat::Rgba8Unorm);
      status.hasError()) {
    return std::move(status).error();
  }

  ShaderResult<FunctionBuilder> entry =
      builder.createComputeEntryPoint("cs_main",
                                      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
                                               BuiltinInput::GlobalInvocationId}},
                                      WorkgroupSize{8, 8, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }
  FunctionBuilder fn = std::move(entry).result();

  const IrExpr outputTexture = GetShaderResultOrFail(fn.ref("outputTexture"), LiteralF32(0));
  const IrExpr gid = GetShaderResultOrFail(fn.ref("gid"), LiteralF32(0));
  const IrExpr coords2 = GetShaderResultOrFail(Swizzle(gid, "xy"), LiteralF32(0));
  const IrExpr extent = GetShaderResultOrFail(
      fn.addLet("extent",
                GetShaderResultOrFail(CallBuiltin(BuiltinFn::TextureDimensions, {outputTexture}),
                                      LiteralF32(0))),
      LiteralF32(0));

  const IrExpr inside = GetShaderResultOrFail(
      fn.addLet("inside", GetShaderResultOrFail(
                              CallBuiltin(BuiltinFn::All, {GetShaderResultOrFail(
                                                              Lt(coords2, extent), LiteralF32(0))}),
                              LiteralF32(0))),
      LiteralF32(0));
  const IrExpr overflow = GetShaderResultOrFail(
      fn.addLet("overflow",
                GetShaderResultOrFail(
                    CallBuiltin(BuiltinFn::Any,
                                {GetShaderResultOrFail(Ge(coords2, extent), LiteralF32(0))}),
                    LiteralF32(0))),
      LiteralF32(0));

  EXPECT_THAT(fn.beginIf(GetShaderResultOrFail(
                  And(inside, GetShaderResultOrFail(Not(overflow), LiteralF32(0))), LiteralF32(0))),
              IsShaderOk());
  EXPECT_THAT(
      fn.textureStore(outputTexture,
                      GetShaderResultOrFail(Convert(IrType::Vec2i(), coords2), LiteralF32(0)),
                      GetShaderResultOrFail(
                          ConstructVector(IrType::Vec4f(), {LiteralF32(1.0f), LiteralF32(0.0f),
                                                            LiteralF32(0.0f), LiteralF32(1.0f)}),
                          LiteralF32(0))),
      IsShaderOk());
  EXPECT_THAT(fn.endIf(), IsShaderOk());
  EXPECT_THAT(fn.finish(), IsShaderOk());

  return builder.build();
}

}  // namespace donner::gpu::shader
