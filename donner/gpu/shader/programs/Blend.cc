#include "donner/gpu/shader/programs/Blend.h"

#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {
namespace {

template <typename Body>
ShaderStatus AddFunction(ModuleBuilder& builder, const char* name, std::vector<IrParam> parameters,
                         const IrType& type, Body body) {
  auto result = builder.createFunction(name, std::move(parameters), type);
  if (result.hasError()) {
    return std::move(result).error();
  }
  auto fn = std::move(result).result();
  ErrorLatch e;
  body(e, fn);
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

IrExpr Vector(ErrorLatch& e, const IrExpr& value) {
  return e(ConstructVector(IrType::Vec3f(), {value}));
}

IrExpr Extremum(ErrorLatch& e, BuiltinFn operation, const IrExpr& color) {
  return e(CallBuiltin(
      operation, {e(Swizzle(color, "x")),
                  e(CallBuiltin(operation, {e(Swizzle(color, "y")), e(Swizzle(color, "z"))}))}));
}

void ReturnIf(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& condition, const IrExpr& value) {
  e.ok(fn.beginIf(condition));
  e.ok(fn.returnValue(value));
  e.ok(fn.endIf());
}

ShaderStatus AddLuminosity(ModuleBuilder& builder) {
  return AddFunction(
      builder, "luminosity", {{"c", IrType::Vec3f()}}, IrType::F32(),
      [](ErrorLatch& e, FunctionBuilder& fn) {
        const auto c = e(fn.ref("c"));
        e.ok(fn.returnValue(e(Add(e(Add(e(Mul(LiteralF32(0.3f), e(Swizzle(c, "x")))),
                                        e(Mul(LiteralF32(0.59f), e(Swizzle(c, "y")))))),
                                  e(Mul(LiteralF32(0.11f), e(Swizzle(c, "z"))))))));
      });
}

ShaderStatus AddClipColor(ModuleBuilder& builder) {
  return AddFunction(
      builder, "clipColor", {{"inputColor", IrType::Vec3f()}}, IrType::Vec3f(),
      [](ErrorLatch& e, FunctionBuilder& fn) {
        const auto input = e(fn.ref("inputColor"));
        const auto l = e(fn.addLet("l", e(fn.callFunction("luminosity", {input}))));
        const auto n = e(fn.addLet("n", Extremum(e, BuiltinFn::Min, input)));
        const auto x = e(fn.addLet("x", Extremum(e, BuiltinFn::Max, input)));
        const auto color = e(fn.addVar("color", IrType::Vec3f(), input));
        const auto lv = Vector(e, l);
        e.ok(fn.beginIf(e(And(e(Lt(n, LiteralF32(0))), e(Ne(l, n))))));
        e.ok(fn.assign(color, e(Add(lv, e(Div(e(Mul(e(Sub(color, lv)), l)), e(Sub(l, n))))))));
        e.ok(fn.endIf());
        e.ok(fn.beginIf(e(And(e(Gt(x, LiteralF32(1))), e(Ne(x, l))))));
        e.ok(fn.assign(
            color,
            e(Add(lv, e(Div(e(Mul(e(Sub(color, lv)), e(Sub(LiteralF32(1), l)))), e(Sub(x, l))))))));
        e.ok(fn.endIf());
        e.ok(fn.returnValue(color));
      });
}

ShaderStatus AddSetLuminosity(ModuleBuilder& builder) {
  return AddFunction(
      builder, "setLuminosity", {{"c", IrType::Vec3f()}, {"l", IrType::F32()}}, IrType::Vec3f(),
      [](ErrorLatch& e, FunctionBuilder& fn) {
        const auto c = e(fn.ref("c"));
        const auto delta = e(Sub(e(fn.ref("l")), e(fn.callFunction("luminosity", {c}))));
        e.ok(fn.returnValue(e(fn.callFunction("clipColor", {e(Add(c, Vector(e, delta)))}))));
      });
}

ShaderStatus AddSetSaturation(ModuleBuilder& builder) {
  return AddFunction(
      builder, "setSaturation", {{"c", IrType::Vec3f()}, {"s", IrType::F32()}}, IrType::Vec3f(),
      [](ErrorLatch& e, FunctionBuilder& fn) {
        const auto c = e(fn.ref("c"));
        const auto low = e(fn.addLet("low", Extremum(e, BuiltinFn::Min, c)));
        const auto high = e(fn.addLet("high", Extremum(e, BuiltinFn::Max, c)));
        ReturnIf(e, fn, e(Le(high, low)), Vector(e, LiteralF32(0)));
        const auto scaled =
            e(Div(e(Mul(e(Sub(c, Vector(e, low))), e(fn.ref("s")))), e(Sub(high, low))));
        const auto upper = e(CallBuiltin(
            BuiltinFn::Select, {scaled, Vector(e, e(fn.ref("s"))), e(Eq(c, Vector(e, high)))}));
        e.ok(fn.returnValue(e(CallBuiltin(
            BuiltinFn::Select, {upper, Vector(e, LiteralF32(0)), e(Eq(c, Vector(e, low)))}))));
      });
}

ShaderStatus AddDodge(ModuleBuilder& builder) {
  return AddFunction(builder, "dodge", {{"b", IrType::F32()}, {"s", IrType::F32()}}, IrType::F32(),
                     [](ErrorLatch& e, FunctionBuilder& fn) {
                       const auto b = e(fn.ref("b")), s = e(fn.ref("s"));
                       ReturnIf(e, fn, e(Eq(b, LiteralF32(0))), LiteralF32(0));
                       ReturnIf(e, fn, e(Ge(s, LiteralF32(1))), LiteralF32(1));
                       e.ok(fn.returnValue(e(CallBuiltin(
                           BuiltinFn::Min, {LiteralF32(1), e(Div(b, e(Sub(LiteralF32(1), s))))}))));
                     });
}

ShaderStatus AddBurn(ModuleBuilder& builder) {
  return AddFunction(
      builder, "burn", {{"b", IrType::F32()}, {"s", IrType::F32()}}, IrType::F32(),
      [](ErrorLatch& e, FunctionBuilder& fn) {
        const auto b = e(fn.ref("b")), s = e(fn.ref("s"));
        ReturnIf(e, fn, e(Ge(b, LiteralF32(1))), LiteralF32(1));
        ReturnIf(e, fn, e(Le(s, LiteralF32(0))), LiteralF32(0));
        e.ok(fn.returnValue(e(Sub(
            LiteralF32(1), e(CallBuiltin(BuiltinFn::Min,
                                         {LiteralF32(1), e(Div(e(Sub(LiteralF32(1), b)), s))}))))));
      });
}

ShaderStatus AddSoftLight(ModuleBuilder& builder) {
  return AddFunction(
      builder, "softLight", {{"b", IrType::F32()}, {"s", IrType::F32()}}, IrType::F32(),
      [](ErrorLatch& e, FunctionBuilder& fn) {
        const auto b = e(fn.ref("b")), s = e(fn.ref("s"));
        ReturnIf(e, fn, e(Le(s, LiteralF32(0.5f))),
                 e(Sub(b, e(Mul(e(Mul(e(Sub(LiteralF32(1), e(Mul(LiteralF32(2), s)))), b)),
                                e(Sub(LiteralF32(1), b)))))));
        const auto d = e(fn.addVar("d", IrType::F32(), LiteralF32(0)));
        e.ok(fn.beginIf(e(Le(b, LiteralF32(0.25f)))));
        e.ok(fn.assign(d, e(Mul(e(Add(e(Mul(e(Sub(e(Mul(LiteralF32(16), b)), LiteralF32(12))), b)),
                                      LiteralF32(4))),
                                b))));
        e.ok(fn.elseBranch());
        e.ok(fn.assign(d, e(CallBuiltin(BuiltinFn::Sqrt, {b}))));
        e.ok(fn.endIf());
        e.ok(fn.returnValue(
            e(Add(b, e(Mul(e(Sub(e(Mul(LiteralF32(2), s)), LiteralF32(1))), e(Sub(d, b))))))));
      });
}

IrExpr HardLight(ErrorLatch& e, const IrExpr& b, const IrExpr& s) {
  const auto low = e(Mul(e(Mul(LiteralF32(2), b)), s));
  const auto high =
      e(Sub(LiteralF32(1),
            e(Mul(e(Mul(LiteralF32(2), e(Sub(LiteralF32(1), b)))), e(Sub(LiteralF32(1), s))))));
  return e(CallBuiltin(BuiltinFn::Select, {high, low, e(Le(s, LiteralF32(0.5f)))}));
}

ShaderStatus AddSeparable(ModuleBuilder& builder) {
  return AddFunction(builder, "separable",
                     {{"mode", IrType::U32()}, {"b", IrType::F32()}, {"s", IrType::F32()}},
                     IrType::F32(), [](ErrorLatch& e, FunctionBuilder& fn) {
                       const auto b = e(fn.ref("b")), s = e(fn.ref("s")), mode = e(fn.ref("mode"));
                       const std::vector<std::pair<uint32_t, IrExpr>> cases{
                           {1, e(Mul(b, s))},
                           {2, e(Sub(e(Add(b, s)), e(Mul(b, s))))},
                           {3, e(CallBuiltin(BuiltinFn::Min, {b, s}))},
                           {4, e(CallBuiltin(BuiltinFn::Max, {b, s}))},
                           {5, HardLight(e, s, b)},
                           {6, e(fn.callFunction("dodge", {b, s}))},
                           {7, e(fn.callFunction("burn", {b, s}))},
                           {8, HardLight(e, b, s)},
                           {9, e(fn.callFunction("softLight", {b, s}))},
                           {10, e(CallBuiltin(BuiltinFn::Abs, {e(Sub(b, s))}))},
                           {11, e(Sub(e(Add(b, s)), e(Mul(e(Mul(LiteralF32(2), b)), s))))}};
                       for (const auto& [index, value] : cases) {
                         ReturnIf(e, fn, e(Eq(mode, LiteralU32(index))), value);
                       }
                       e.ok(fn.returnValue(s));
                     });
}

ShaderStatus AddBlendFunction(ModuleBuilder& builder) {
  return AddFunction(
      builder, "blend", {{"mode", IrType::U32()}, {"b", IrType::Vec3f()}, {"s", IrType::Vec3f()}},
      IrType::Vec3f(), [](ErrorLatch& e, FunctionBuilder& fn) {
        const auto b = e(fn.ref("b")), s = e(fn.ref("s")), mode = e(fn.ref("mode"));
        const auto lum = [&](const IrExpr& c) { return e(fn.callFunction("luminosity", {c})); };
        const auto sat = [&](const IrExpr& c) {
          return e(Sub(Extremum(e, BuiltinFn::Max, c), Extremum(e, BuiltinFn::Min, c)));
        };
        const auto setLum = [&](const IrExpr& c, const IrExpr& l) {
          return e(fn.callFunction("setLuminosity", {c, l}));
        };
        ReturnIf(e, fn, e(Eq(mode, LiteralU32(12))),
                 setLum(e(fn.callFunction("setSaturation", {s, sat(b)})), lum(b)));
        ReturnIf(e, fn, e(Eq(mode, LiteralU32(13))),
                 setLum(e(fn.callFunction("setSaturation", {b, sat(s)})), lum(b)));
        ReturnIf(e, fn, e(Eq(mode, LiteralU32(14))), setLum(s, lum(b)));
        ReturnIf(e, fn, e(Eq(mode, LiteralU32(15))), setLum(b, lum(s)));
        std::vector<IrExpr> channels;
        for (const char* channel : {"x", "y", "z"}) {
          channels.push_back(e(fn.callFunction(
              "separable", {mode, e(Swizzle(b, channel)), e(Swizzle(s, channel))})));
        }
        e.ok(fn.returnValue(e(ConstructVector(IrType::Vec3f(), channels))));
      });
}

IrExpr StraightColor(ErrorLatch& e, FunctionBuilder& fn, const char* name, const IrExpr& color) {
  const auto alpha = e(Swizzle(color, "w"));
  const auto result = e(fn.addVar(name, IrType::Vec3f(), Vector(e, LiteralF32(0))));
  e.ok(fn.beginIf(e(Gt(alpha, LiteralF32(0)))));
  e.ok(fn.assign(result, e(Mul(e(Swizzle(color, "xyz")), e(Div(LiteralF32(1), alpha))))));
  e.ok(fn.endIf());
  return result;
}

void AddComposite(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& coord) {
  const auto source = e(fn.addLet(
      "source",
      e(CallBuiltin(BuiltinFn::TextureLoad, {e(fn.ref("sourceTexture")), coord, LiteralI32(0)}))));
  const auto backdrop = e(fn.addLet(
      "backdrop", e(CallBuiltin(BuiltinFn::TextureLoad,
                                {e(fn.ref("destinationTexture")), coord, LiteralI32(0)}))));
  const auto as = e(Swizzle(source, "w")), ab = e(Swizzle(backdrop, "w"));
  const auto mode = e(Member(e(fn.ref("params")), "mode"));
  const auto cs = StraightColor(e, fn, "cs", source), cb = StraightColor(e, fn, "cb", backdrop);
  const auto blended = e(fn.addLet("blended", e(fn.callFunction("blend", {mode, cb, cs}))));
  const auto sourceFactor = e(Mul(e(Sub(LiteralF32(1), ab)), as));
  const auto backdropFactor = e(Mul(e(Sub(LiteralF32(1), as)), ab));
  const auto co = e(Add(e(Add(e(Mul(sourceFactor, cs)), e(Mul(backdropFactor, cb)))),
                        e(Mul(e(Mul(as, ab)), blended))));
  const auto alpha = e(Sub(e(Add(as, ab)), e(Mul(as, ab))));
  const auto result =
      e(fn.addVar("result", IrType::Vec4f(), e(ConstructVector(IrType::Vec4f(), {co, alpha}))));
  e.ok(fn.beginIf(e(Or(e(Eq(mode, LiteralU32(0))), e(Gt(mode, LiteralU32(15)))))));
  e.ok(fn.assign(result, e(Add(source, e(Mul(backdrop, e(Sub(LiteralF32(1), as))))))));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Eq(mode, LiteralU32(1)))));
  e.ok(fn.assign(result, e(Add(e(Add(e(Mul(source, e(Sub(LiteralF32(1), ab)))),
                                     e(Mul(backdrop, e(Sub(LiteralF32(1), as)))))),
                               e(Mul(backdrop, source))))));
  e.ok(fn.endIf());
  e.ok(fn.beginIf(e(Eq(mode, LiteralU32(2)))));
  e.ok(fn.assign(result, e(Sub(e(Add(source, backdrop)), e(Mul(backdrop, source))))));
  e.ok(fn.endIf());
  e.ok(fn.textureStore(
      e(fn.ref("outputTexture")), coord,
      e(CallBuiltin(BuiltinFn::Clamp, {result, e(ConstructVector(IrType::Vec4f(), {LiteralF32(0)})),
                                       e(ConstructVector(IrType::Vec4f(), {LiteralF32(1)}))}))));
}
}  // namespace

ShaderResult<IrModule> BuildBlendModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  e.ok(builder.addTexture2d(0, 0, "sourceTexture"));
  e.ok(builder.addTexture2d(0, 1, "destinationTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, 2, "outputTexture",
                                            StorageTextureFormat::Rgba32Float));
  e.ok(builder.addUniformBuffer(0, 3, "params",
                                e(IrType::Struct("BlendParams", {{"mode", IrType::U32()},
                                                                 {"padding0", IrType::U32()},
                                                                 {"padding1", IrType::U32()},
                                                                 {"padding2", IrType::U32()}}))));
  e.ok(AddLuminosity(builder));
  e.ok(AddClipColor(builder));
  e.ok(AddSetLuminosity(builder));
  e.ok(AddSetSaturation(builder));
  e.ok(AddDodge(builder));
  e.ok(AddBurn(builder));
  e.ok(AddSoftLight(builder));
  e.ok(AddSeparable(builder));
  e.ok(AddBlendFunction(builder));
  auto result = builder.createComputeEntryPoint(
      RcString(kBlendEntryPoint),
      {{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt, BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kBlendWorkgroupSize, kBlendWorkgroupSize, 1});
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
  AddComposite(e, fn, coord);
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}
}  // namespace donner::gpu::shader::programs
