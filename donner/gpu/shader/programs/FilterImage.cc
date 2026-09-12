#include "donner/gpu/shader/programs/FilterImage.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"
#include "donner/gpu/shader/programs/ErrorLatch.h"

namespace donner::gpu::shader::programs {
namespace {

template <typename Body>
ShaderStatus AddFunction(ModuleBuilder& builder, const char* name, std::vector<IrParam> parameters,
                         const IrType& returnType, Body body) {
  ShaderResult<FunctionBuilder> result =
      builder.createFunction(name, std::move(parameters), returnType);
  if (result.hasError()) {
    return std::move(result).error();
  }

  FunctionBuilder fn = std::move(result).result();
  ErrorLatch e;
  body(e, fn);
  e.ok(fn.finish());
  return e.error ? ShaderStatus(*e.error) : OkShaderStatus();
}

IrExpr FloatVector(ErrorLatch& e, const IrType& type, float value) {
  return e(ConstructVector(type, {LiteralF32(value)}));
}

IrExpr IntVector(ErrorLatch& e, int32_t value) {
  return e(ConstructVector(IrType::Vec2i(), {LiteralI32(value)}));
}

IrExpr FloatOffset(ErrorLatch& e, const IrExpr& coord, float x, float y) {
  return e(Add(coord, e(ConstructVector(IrType::Vec2f(), {LiteralF32(x), LiteralF32(y)}))));
}

IrExpr IntOffset(ErrorLatch& e, const IrExpr& coord, int32_t x, int32_t y) {
  return e(Add(coord, e(ConstructVector(IrType::Vec2i(), {LiteralI32(x), LiteralI32(y)}))));
}

IrExpr Mix(ErrorLatch& e, const IrExpr& a, const IrExpr& b, const IrExpr& fraction) {
  return e(Add(a, e(Mul(fraction, e(Sub(b, a))))));
}

ShaderStatus AddSampleImage(ModuleBuilder& builder) {
  return AddFunction(
      builder, "sampleImage", {IrParam{"coord", IrType::Vec2i()}, IrParam{"size", IrType::Vec2i()}},
      IrType::Vec4f(), [](ErrorLatch& e, FunctionBuilder& fn) {
        const IrExpr bounded =
            e(CallBuiltin(BuiltinFn::Clamp, {e(fn.ref("coord")), IntVector(e, 0),
                                             e(Sub(e(fn.ref("size")), IntVector(e, 1)))}));
        e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::TextureLoad,
                                          {e(fn.ref("imageTexture")), bounded, LiteralI32(0)}))));
      });
}

ShaderStatus AddSampleVirtualImage(ModuleBuilder& builder) {
  return AddFunction(
      builder, "sampleVirtualImage",
      {IrParam{"coord", IrType::Vec2f()}, IrParam{"size", IrType::Vec2i()},
       IrParam{"multiple", IrType::Vec2f()}},
      IrType::Vec4f(), [](ErrorLatch& e, FunctionBuilder& fn) {
        const IrExpr virtualSource = e(Convert(
            IrType::Vec2i(),
            e(CallBuiltin(BuiltinFn::Floor, {e(Div(e(fn.ref("coord")), e(fn.ref("multiple"))))}))));
        const IrExpr bounded =
            e(CallBuiltin(BuiltinFn::Clamp, {virtualSource, IntVector(e, 0),
                                             e(Sub(e(fn.ref("size")), IntVector(e, 1)))}));
        e.ok(fn.returnValue(e(fn.callFunction("sampleImage", {bounded, e(fn.ref("size"))}))));
      });
}

ShaderStatus AddPixelatedSample(ModuleBuilder& builder) {
  return AddFunction(
      builder, "samplePixelated",
      {IrParam{"position", IrType::Vec2f()}, IrParam{"size", IrType::Vec2i()},
       IrParam{"multiple", IrType::Vec2f()}},
      IrType::Vec4f(), [](ErrorLatch& e, FunctionBuilder& fn) {
        const IrExpr multiple = e(fn.ref("multiple"));
        const IrExpr virtualPosition = e(fn.addLet(
            "virtualPosition",
            e(Sub(e(Mul(e(Add(e(fn.ref("position")), FloatVector(e, IrType::Vec2f(), 0.5f))),
                        multiple)),
                  FloatVector(e, IrType::Vec2f(), 0.5f)))));
        const IrExpr base =
            e(fn.addLet("base", e(CallBuiltin(BuiltinFn::Floor, {virtualPosition}))));
        const IrExpr fraction = e(fn.addLet("fraction", e(Sub(virtualPosition, base))));
        const auto sample = [&](float x, float y) {
          return e(fn.callFunction("sampleVirtualImage",
                                   {FloatOffset(e, base, x, y), e(fn.ref("size")), multiple}));
        };
        const IrExpr top =
            e(fn.addLet("top", Mix(e, sample(0, 0), sample(1, 0), e(Swizzle(fraction, "x")))));
        const IrExpr bottom =
            e(fn.addLet("bottom", Mix(e, sample(0, 1), sample(1, 1), e(Swizzle(fraction, "x")))));
        e.ok(fn.returnValue(Mix(e, top, bottom, e(Swizzle(fraction, "y")))));
      });
}

ShaderStatus AddCubicWeight(ModuleBuilder& builder) {
  return AddFunction(
      builder, "cubicWeight", {IrParam{"inputValue", IrType::F32()}}, IrType::F32(),
      [](ErrorLatch& e, FunctionBuilder& fn) {
        const IrExpr distance =
            e(fn.addLet("distance", e(CallBuiltin(BuiltinFn::Abs, {e(fn.ref("inputValue"))}))));
        const IrExpr b = e(fn.addLet("b", e(Div(LiteralF32(1), LiteralF32(3)))));
        const IrExpr c = b;
        const auto scaled = [&](float scale, const IrExpr& value) {
          return e(Mul(LiteralF32(scale), value));
        };
        const auto polynomial = [&](const IrExpr& a3, const IrExpr& a2, const IrExpr& a1,
                                    const IrExpr& a0) {
          const IrExpr distanceSquared = e(Mul(distance, distance));
          const IrExpr distanceCubed = e(Mul(distanceSquared, distance));
          return e(Div(e(Add(e(Add(e(Add(e(Mul(a3, distanceCubed)), e(Mul(a2, distanceSquared)))),
                                   e(Mul(a1, distance)))),
                             a0)),
                       LiteralF32(6)));
        };

        e.ok(fn.beginIf(e(Lt(distance, LiteralF32(1)))));
        e.ok(fn.returnValue(polynomial(e(Sub(e(Sub(LiteralF32(12), scaled(9, b))), scaled(6, c))),
                                       e(Add(e(Add(LiteralF32(-18), scaled(12, b))), scaled(6, c))),
                                       LiteralF32(0), e(Sub(LiteralF32(6), scaled(2, b))))));
        e.ok(fn.endIf());
        e.ok(fn.beginIf(e(Lt(distance, LiteralF32(2)))));
        e.ok(fn.returnValue(polynomial(
            e(Sub(scaled(-1, b), scaled(6, c))), e(Add(scaled(6, b), scaled(30, c))),
            e(Sub(scaled(-12, b), scaled(48, c))), e(Add(scaled(8, b), scaled(24, c))))));
        e.ok(fn.endIf());
        e.ok(fn.returnValue(LiteralF32(0)));
      });
}

ShaderStatus AddSmoothSample(ModuleBuilder& builder) {
  return AddFunction(
      builder, "sampleSmooth",
      {IrParam{"position", IrType::Vec2f()}, IrParam{"size", IrType::Vec2i()}}, IrType::Vec4f(),
      [](ErrorLatch& e, FunctionBuilder& fn) {
        const IrExpr position = e(fn.ref("position"));
        const IrExpr size = e(fn.ref("size"));
        const IrExpr base = e(fn.addLet(
            "base", e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::Floor, {position}))))));
        std::vector<IrExpr> weightsX;
        std::vector<IrExpr> weightsY;
        for (int32_t tap = -1; tap <= 2; ++tap) {
          const auto weight = [&](const char* axis) {
            return e(fn.callFunction(
                "cubicWeight",
                {e(Sub(
                    e(Swizzle(position, axis)),
                    e(Convert(IrType::F32(), e(Add(e(Swizzle(base, axis)), LiteralI32(tap)))))))}));
          };
          weightsX.push_back(
              e(fn.addLet(RcString("weightX" + std::to_string(tap + 1)), weight("x"))));
          weightsY.push_back(
              e(fn.addLet(RcString("weightY" + std::to_string(tap + 1)), weight("y"))));
        }

        const IrExpr accumulated =
            e(fn.addVar("accumulated", IrType::Vec4f(), FloatVector(e, IrType::Vec4f(), 0)));
        for (int32_t y = -1; y <= 2; ++y) {
          const IrExpr row = e(fn.addVar(RcString("row" + std::to_string(y + 1)), IrType::Vec4f(),
                                         FloatVector(e, IrType::Vec4f(), 0)));
          for (int32_t x = -1; x <= 2; ++x) {
            const IrExpr sample =
                e(fn.callFunction("sampleImage", {IntOffset(e, base, x, y), size}));
            e.ok(fn.assign(row, e(Add(row, e(Mul(sample, weightsX[x + 1]))))));
          }
          e.ok(fn.assign(accumulated, e(Add(accumulated, e(Mul(row, weightsY[y + 1]))))));
        }
        e.ok(fn.returnValue(accumulated));
      });
}

IrExpr SourcePosition(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& coord) {
  const IrExpr params = e(fn.ref("params"));
  const IrExpr center = e(fn.addLet(
      "center", e(Add(e(Convert(IrType::Vec2f(), coord)), FloatVector(e, IrType::Vec2f(), 0.5f)))));
  const auto row = [&](const char* xCoefficient, const char* yCoefficient, const char* offset) {
    return e(Sub(e(Add(e(Add(e(Mul(e(Member(params, xCoefficient)), e(Swizzle(center, "x")))),
                             e(Mul(e(Member(params, yCoefficient)), e(Swizzle(center, "y")))))),
                       e(Member(params, offset)))),
                 LiteralF32(0.5f)));
  };
  return e(fn.addLet("position", e(ConstructVector(IrType::Vec2f(), {row("m00", "m01", "m02"),
                                                                     row("m10", "m11", "m12")}))));
}

void Store(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& coord, const IrExpr& color) {
  e.ok(fn.textureStore(e(fn.ref("outputTexture")), coord, color));
}

void SampleOutput(ErrorLatch& e, FunctionBuilder& fn, const IrExpr& coord,
                  const IrExpr& imageSize) {
  const IrExpr position = SourcePosition(e, fn, coord);
  const IrExpr low =
      e(CallBuiltin(BuiltinFn::Any, {e(Lt(position, FloatVector(e, IrType::Vec2f(), -0.5f)))}));
  const IrExpr high = e(CallBuiltin(
      BuiltinFn::Any, {e(Ge(position, e(Sub(e(Convert(IrType::Vec2f(), imageSize)),
                                            FloatVector(e, IrType::Vec2f(), 0.5f)))))}));
  e.ok(fn.beginIf(e(Or(low, high))));
  Store(e, fn, coord, FloatVector(e, IrType::Vec4f(), 0));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr params = e(fn.ref("params"));
  const IrExpr mode = e(Member(params, "samplingMode"));
  const IrExpr sampled =
      e(fn.addVar("sampled", IrType::Vec4f(), FloatVector(e, IrType::Vec4f(), 0)));
  e.ok(fn.beginIf(e(Eq(mode, LiteralU32(1)))));
  const IrExpr nearest = e(Convert(
      IrType::Vec2i(),
      e(CallBuiltin(BuiltinFn::Floor, {e(Add(position, FloatVector(e, IrType::Vec2f(), 0.5f)))}))));
  e.ok(fn.assign(sampled, e(fn.callFunction("sampleImage", {nearest, imageSize}))));
  e.ok(fn.elseBranch());
  e.ok(fn.beginIf(e(Eq(mode, LiteralU32(2)))));
  const IrExpr scales = e(ConstructVector(IrType::Vec2f(), {e(Member(params, "pixelatedScaleX")),
                                                            e(Member(params, "pixelatedScaleY"))}));
  const IrExpr multiple = e(CallBuiltin(
      BuiltinFn::Clamp,
      {e(CallBuiltin(BuiltinFn::Floor, {e(Add(scales, FloatVector(e, IrType::Vec2f(), 0.5f)))})),
       FloatVector(e, IrType::Vec2f(), 1), FloatVector(e, IrType::Vec2f(), 65536)}));
  e.ok(fn.assign(sampled, e(fn.callFunction("samplePixelated", {position, imageSize, multiple}))));
  e.ok(fn.elseBranch());
  e.ok(fn.assign(sampled, e(fn.callFunction("sampleSmooth", {position, imageSize}))));
  e.ok(fn.endIf());
  e.ok(fn.endIf());

  const IrExpr clamped = e(fn.addLet(
      "clamped", e(CallBuiltin(BuiltinFn::Clamp, {sampled, FloatVector(e, IrType::Vec4f(), 0),
                                                  FloatVector(e, IrType::Vec4f(), 1)}))));
  const IrExpr alpha = e(Swizzle(clamped, "w"));
  Store(e, fn, coord,
        e(ConstructVector(
            IrType::Vec4f(),
            {e(CallBuiltin(BuiltinFn::Min, {e(Swizzle(clamped, "xyz")),
                                            e(ConstructVector(IrType::Vec3f(), {alpha}))})),
             alpha})));
}

}  // namespace

ShaderResult<IrModule> BuildFilterImageModule() {
  ErrorLatch e;
  ModuleBuilder builder;
  const auto binding = [](FilterImageBinding value) { return static_cast<uint32_t>(value); };
  const IrType paramsType = e(IrType::Struct(
      "ImageParams", {IrType::Member{"m00", IrType::F32()}, IrType::Member{"m01", IrType::F32()},
                      IrType::Member{"m02", IrType::F32()}, IrType::Member{"m10", IrType::F32()},
                      IrType::Member{"m11", IrType::F32()}, IrType::Member{"m12", IrType::F32()},
                      IrType::Member{"samplingMode", IrType::U32()},
                      IrType::Member{"pixelatedScaleX", IrType::F32()},
                      IrType::Member{"pixelatedScaleY", IrType::F32()},
                      IrType::Member{"padding", IrType::U32()}}));
  e.ok(builder.addTexture2d(0, binding(FilterImageBinding::ImageTexture), "imageTexture"));
  e.ok(builder.addWriteOnlyStorageTexture2d(0, binding(FilterImageBinding::OutputTexture),
                                            "outputTexture", StorageTextureFormat::Rgba32Float));
  e.ok(builder.addUniformBuffer(0, binding(FilterImageBinding::Params), "params", paramsType));
  e.ok(AddSampleImage(builder));
  e.ok(AddSampleVirtualImage(builder));
  e.ok(AddPixelatedSample(builder));
  e.ok(AddCubicWeight(builder));
  e.ok(AddSmoothSample(builder));

  ShaderResult<FunctionBuilder> entry = builder.createComputeEntryPoint(
      RcString(kFilterImageEntryPoint),
      {IrParam{"gid", IrType::Vec3(ScalarKind::U32), std::nullopt,
               BuiltinInput::GlobalInvocationId}},
      WorkgroupSize{kFilterImageWorkgroupSize, kFilterImageWorkgroupSize, 1});
  if (entry.hasError()) {
    return std::move(entry).error();
  }

  FunctionBuilder fn = std::move(entry).result();
  const IrExpr coord =
      e(fn.addLet("coord", e(Convert(IrType::Vec2i(), e(Swizzle(e(fn.ref("gid")), "xy"))))));
  const IrExpr outputSize = e(fn.addLet(
      "outputSize", e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions,
                                                             {e(fn.ref("outputTexture"))}))))));
  e.ok(fn.beginIf(e(CallBuiltin(BuiltinFn::Any, {e(Ge(coord, outputSize))}))));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());

  const IrExpr imageSize = e(fn.addLet(
      "imageSize", e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions,
                                                            {e(fn.ref("imageTexture"))}))))));
  e.ok(fn.beginIf(e(CallBuiltin(BuiltinFn::Any, {e(Le(imageSize, IntVector(e, 0)))}))));
  Store(e, fn, coord, FloatVector(e, IrType::Vec4f(), 0));
  e.ok(fn.returnVoid());
  e.ok(fn.endIf());
  SampleOutput(e, fn, coord, imageSize);
  e.ok(fn.finish());
  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
