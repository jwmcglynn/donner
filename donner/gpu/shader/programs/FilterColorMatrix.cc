#include "donner/gpu/shader/programs/FilterColorMatrix.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {

namespace {

/// Binding index of \p binding as the module builder takes it.
uint32_t BindingIndex(FilterColorMatrixBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Adds the three module-scope bindings the entry point reads and writes.
ShaderStatus AddBindings(ModuleBuilder& builder, const IrType& paramsType) {
  if (ShaderStatus status = builder.addTexture2d(
          0, BindingIndex(FilterColorMatrixBinding::InputTexture), "inputTexture");
      status.hasError()) {
    return status;
  }
  if (ShaderStatus status = builder.addWriteOnlyStorageTexture2d(
          0, BindingIndex(FilterColorMatrixBinding::OutputTexture), "outputTexture",
          StorageTextureFormat::Rgba8Unorm);
      status.hasError()) {
    return status;
  }
  return builder.addUniformBuffer(0, BindingIndex(FilterColorMatrixBinding::Params), "params",
                                  paramsType);
}

/// `vec4<f32>(color.xyz * alpha, alpha)`: a straight-alpha color reassociated with \p alpha.
/// @param e Error latch. @param color Straight-alpha color. @param alpha Alpha to premultiply by.
IrExpr Premultiplied(ErrorLatch& e, const IrExpr& color, const IrExpr& alpha) {
  return e(ConstructVector(IrType::Vec4f(), {e(Mul(e(Swizzle(color, "xyz")), alpha)), alpha}));
}

}  // namespace

ShaderResult<IrModule> BuildFilterColorMatrixModule() {
  ErrorLatch e;
  ModuleBuilder builder;

  const IrType vec4f = IrType::Vec4f();
  const IrType paramsType = e(IrType::Struct(
      "FilterColorMatrixParams",
      {IrType::Member{"col0", vec4f}, IrType::Member{"col1", vec4f}, IrType::Member{"col2", vec4f},
       IrType::Member{"col3", vec4f}, IrType::Member{"col4", vec4f}}));
  e.ok(AddBindings(builder, paramsType));

  auto entryResult = builder.createComputeEntryPoint(
      "cs_main",
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kFilterColorMatrixWorkgroupSize, kFilterColorMatrixWorkgroupSize, 1});
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

  const IrExpr params = e(fn.ref("params"));
  const IrExpr transparentBlack = e(ConstructVector(vec4f, {LiteralF32(0.0f)}));

  // The matrix is defined on straight-alpha values while the filter chain carries premultiplied
  // ones, so the source is divided through by its alpha first.
  const IrExpr straight = e(fn.addVar("straight", vec4f, transparentBlack));
  e.ok(fn.beginIf(e(Gt(e(Swizzle(source, "w")), LiteralF32(0.0f)))));
  e.ok(fn.assign(straight, e(ConstructVector(
                               vec4f, {e(Div(e(Swizzle(source, "xyz")), e(Swizzle(source, "w")))),
                                       e(Swizzle(source, "w"))}))));
  e.ok(fn.elseBranch());
  {
    // A fully transparent texel has no straight-alpha color to recover, so the constant column is
    // the whole result: the four multiplier columns would each be scaled by a zero channel.
    const IrExpr offsetAlpha = e(
        fn.addLet("offsetAlpha", e(CallBuiltin(BuiltinFn::Saturate, {e(Member(params, "col4"))}))));
    e.ok(fn.beginIf(e(Eq(e(Swizzle(offsetAlpha, "w")), LiteralF32(0.0f)))));
    e.ok(fn.textureStore(outputTexture, coords, transparentBlack));
    e.ok(fn.returnVoid());
    e.ok(fn.endIf());
    e.ok(fn.textureStore(outputTexture, coords,
                         Premultiplied(e, offsetAlpha, e(Swizzle(offsetAlpha, "w")))));
    e.ok(fn.returnVoid());
  }
  e.ok(fn.endIf());

  const IrExpr weighted = e(fn.addLet(
      "weighted",
      e(Add(e(Add(e(Add(e(Add(e(Mul(e(Member(params, "col0")), e(Swizzle(straight, "x")))),
                              e(Mul(e(Member(params, "col1")), e(Swizzle(straight, "y")))))),
                        e(Mul(e(Member(params, "col2")), e(Swizzle(straight, "z")))))),
                  e(Mul(e(Member(params, "col3")), e(Swizzle(straight, "w")))))),
            e(Member(params, "col4"))))));
  // Saturate is the specification's clamp to the representable range, and it is also what makes
  // the premultiply below produce a color no greater than its own alpha.
  const IrExpr clamped = e(fn.addLet("clamped", e(CallBuiltin(BuiltinFn::Saturate, {weighted}))));
  e.ok(fn.textureStore(outputTexture, coords, Premultiplied(e, clamped, e(Swizzle(clamped, "w")))));
  e.ok(fn.finish());

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
