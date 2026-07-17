#include "donner/gpu/shader/programs/SolidFill.h"

#include <optional>
#include <utility>
#include <vector>

#include "donner/gpu/shader/IrExpr.h"

namespace donner::gpu::shader::programs {

namespace {

/**
 * Latches the first builder error so the program can be transliterated linearly. On error every
 * subsequent expression receives a dummy `0.0f`; the resulting cascade errors are ignored
 * because only the first is reported. The inputs are static, so any latched error is a Donner
 * bug surfaced by the golden test, never a runtime condition.
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

/// Shorthand scalar literals.
IrExpr F(float value) {
  return LiteralF32(value);
}
IrExpr U(uint32_t value) {
  return LiteralU32(value);
}
IrExpr I(int32_t value) {
  return LiteralI32(value);
}

/// Per-axis configuration for accumulateHoriz / accumulateVert (the vertical ray is the
/// transpose of the horizontal one).
struct RayConfig {
  const char* functionName;   //!< accumulateHoriz / accumulateVert.
  const char* ppemParamName;  //!< ppemX / ppemY.
  const char* bandsBinding;   //!< bands / vBands.
  const char* loaderName;     //!< load_h_curve / load_v_curve.
  const char* solveAxis;      //!< Monotone axis solved against: "y" (horiz) or "x" (vert).
  const char* evalAxis;       //!< Axis the crossing position is evaluated on: "x" or "y".
  float signWhenPositive;     //!< Winding sign when d(solveAxis)/dt >= 0.
  float signWhenNegative;     //!< Winding sign otherwise.
};

/// Builds one of the two curve loaders (load_h_curve / load_v_curve), which unpack six floats
/// from the given curve data binding into a Quadratic.
void BuildCurveLoader(Latch& e, ModuleBuilder& builder, const IrType& quadraticType,
                      const char* name, const char* curveDataBinding) {
  auto result = builder.createFunction(name, {IrParam{"index", IrType::U32()}}, quadraticType);
  if (result.hasError()) {
    e.ok(ShaderStatus(std::move(result).error()));
    return;
  }
  FunctionBuilder fn = std::move(result).result();

  const IrExpr index = e(fn.ref("index"));
  const IrExpr curveData = e(fn.ref(curveDataBinding));
  const IrExpr base = e(fn.addLet("base", e(Mul(index, U(6)))));
  const IrExpr q = e(fn.addVar("q", quadraticType));

  const auto point = [&](uint32_t offset0, uint32_t offset1) {
    return e(ConstructVector(IrType::Vec2f(), {e(Index(curveData, e(Add(base, U(offset0))))),
                                               e(Index(curveData, e(Add(base, U(offset1)))))}));
  };
  e.ok(fn.assign(e(Member(q, "p0")), point(0, 1)));
  e.ok(fn.assign(e(Member(q, "p1")), point(2, 3)));
  e.ok(fn.assign(e(Member(q, "p2")), point(4, 5)));
  e.ok(fn.returnValue(q));
  e.ok(fn.finish());
}

/// Builds accumulateHoriz or accumulateVert: casts one axis-aligned ray through the pixel's
/// band and accumulates signed analytic coverage per crossing (design 0041, Slug CalcCoverage).
void BuildAccumulate(Latch& e, ModuleBuilder& builder, const IrType& rayCoverageType,
                     const RayConfig& config) {
  auto result =
      builder.createFunction(config.functionName,
                             {IrParam{"slot", IrType::U32()}, IrParam{"sample", IrType::Vec2f()},
                              IrParam{config.ppemParamName, IrType::F32()}},
                             rayCoverageType);
  if (result.hasError()) {
    e.ok(ShaderStatus(std::move(result).error()));
    return;
  }
  FunctionBuilder fn = std::move(result).result();

  const IrExpr slot = e(fn.ref("slot"));
  const IrExpr sample = e(fn.ref("sample"));
  const IrExpr ppem = e(fn.ref(config.ppemParamName));

  const IrExpr resultVar = e(fn.addVar("result", rayCoverageType));
  e.ok(fn.assign(e(Member(resultVar, "cov")), F(0.0f)));
  e.ok(fn.assign(e(Member(resultVar, "wgt")), F(0.0f)));

  e.ok(fn.beginIf(e(Eq(slot, e(fn.ref("kNoBand"))))));
  e.ok(fn.returnValue(resultVar));
  e.ok(fn.endIf());

  const IrExpr band = e(fn.addLet("band", e(Index(e(fn.ref(config.bandsBinding)), slot))));

  const IrExpr i = e(fn.beginFor("i", U(0)));
  e.ok(fn.forCondition(e(Lt(i, e(Member(band, "curveCount"))))));
  e.ok(fn.forContinuing(i, e(Add(i, U(1)))));
  {
    const IrExpr curve = e(fn.addLet(
        "curve",
        e(fn.callFunction(config.loaderName, {e(Add(e(Member(band, "curveStart")), i))}))));

    const auto curvePoint = [&](const char* pointName, const char* axis) {
      return e(Swizzle(e(Member(curve, pointName)), axis));
    };
    const IrExpr sampleOnSolveAxis = e(Swizzle(sample, config.solveAxis));

    e.ok(fn.beginIf(e(Not(e(fn.callFunction(
        "owns_axis_sample", {curvePoint("p0", config.solveAxis), curvePoint("p2", config.solveAxis),
                             sampleOnSolveAxis}))))));
    e.ok(fn.continueStmt());
    e.ok(fn.endIf());

    // Quadratic coefficients along the solve axis: a t^2 + b t + c = 0 at the sample.
    const IrExpr a =
        e(fn.addLet("a", e(Add(e(Sub(curvePoint("p0", config.solveAxis),
                                     e(Mul(F(2.0f), curvePoint("p1", config.solveAxis))))),
                               curvePoint("p2", config.solveAxis)))));
    const IrExpr b = e(fn.addLet("b", e(Mul(F(2.0f), e(Sub(curvePoint("p1", config.solveAxis),
                                                           curvePoint("p0", config.solveAxis)))))));
    const IrExpr c =
        e(fn.addLet("c", e(Sub(curvePoint("p0", config.solveAxis), sampleOnSolveAxis))));
    const IrExpr roots = e(fn.addLet("roots", e(fn.callFunction("solve_quadratic", {a, b, c}))));

    const IrExpr k = e(fn.beginFor("k", I(0)));
    e.ok(fn.forCondition(e(Lt(k, I(2)))));
    e.ok(fn.forContinuing(k, e(Add(k, I(1)))));
    {
      const IrExpr t = e(fn.addLet(
          "t", e(CallBuiltin(BuiltinFn::Select,
                             {e(Swizzle(roots, "y")), e(Swizzle(roots, "x")), e(Eq(k, I(0)))}))));
      e.ok(fn.beginIf(e(Lt(t, F(0.0f)))));
      e.ok(fn.continueStmt());
      e.ok(fn.endIf());

      const IrExpr omt = e(fn.addLet("omt", e(Sub(F(1.0f), t))));
      // Evaluate the crossing position on the evaluation axis:
      // omt^2 * p0 + 2 omt t * p1 + t^2 * p2.
      const IrExpr crossing = e(fn.addLet(
          config.evalAxis,
          e(Add(e(Add(e(Mul(e(Mul(omt, omt)), curvePoint("p0", config.evalAxis))),
                      e(Mul(e(Mul(e(Mul(F(2.0f), omt)), t)), curvePoint("p1", config.evalAxis))))),
                e(Mul(e(Mul(t, t)), curvePoint("p2", config.evalAxis)))))));
      // Signed pixel distance of the crossing from the pixel center.
      const IrExpr r =
          e(fn.addLet("r", e(Mul(e(Sub(crossing, e(Swizzle(sample, config.evalAxis)))), ppem))));
      const IrExpr derivative = e(fn.addLet(
          "d_dt", e(Add(e(Mul(e(Mul(F(2.0f), omt)), e(Sub(curvePoint("p1", config.solveAxis),
                                                          curvePoint("p0", config.solveAxis))))),
                        e(Mul(e(Mul(F(2.0f), t)), e(Sub(curvePoint("p2", config.solveAxis),
                                                        curvePoint("p1", config.solveAxis)))))))));
      const IrExpr sign = e(fn.addLet(
          "s",
          e(CallBuiltin(BuiltinFn::Select, {F(config.signWhenNegative), F(config.signWhenPositive),
                                            e(Ge(derivative, F(0.0f)))}))));

      e.ok(fn.assign(
          e(Member(resultVar, "cov")),
          e(Add(e(Member(resultVar, "cov")),
                e(Mul(sign, e(CallBuiltin(BuiltinFn::Saturate, {e(Add(r, F(0.5f)))}))))))));
      e.ok(fn.assign(
          e(Member(resultVar, "wgt")),
          e(CallBuiltin(BuiltinFn::Max,
                        {e(Member(resultVar, "wgt")),
                         e(CallBuiltin(BuiltinFn::Saturate,
                                       {e(Sub(F(1.0f), e(Mul(e(CallBuiltin(BuiltinFn::Abs, {r})),
                                                             F(2.0f)))))}))}))));
    }
    e.ok(fn.endFor());
  }
  e.ok(fn.endFor());

  e.ok(fn.returnValue(resultVar));
  e.ok(fn.finish());
}

}  // namespace

ShaderResult<IrModule> BuildSolidFillModule() {
  Latch e;
  const IrType f32 = IrType::F32();
  const IrType u32 = IrType::U32();
  const IrType vec2f = IrType::Vec2f();
  const IrType vec4f = IrType::Vec4f();

  // ----- Struct types (byte layouts anchored by the packet 4 layout tests) -----
  const IrType planesArray = e(IrType::SizedArray(vec4f, 4));
  const IrType uniformsType =
      e(IrType::Struct("Uniforms", {
                                       {"mvp", IrType::Mat4x4f()},
                                       {"patternFromPath", IrType::Mat4x4f()},
                                       {"viewport", vec2f},
                                       {"tileSize", vec2f},
                                       {"color", vec4f},
                                       {"fillRule", u32},
                                       {"paintMode", u32},
                                       {"patternOpacity", f32},
                                       {"hasClipPolygon", u32},
                                       {"hasClipMask", u32},
                                       {"_pad0", u32},
                                       {"_pad1", u32},
                                       {"_pad2", u32},
                                       {"yBase", f32},
                                       {"hStride", f32},
                                       {"hBandCount", u32},
                                       {"xBase", f32},
                                       {"vStride", f32},
                                       {"vBandCount", u32},
                                       {"_gridPad0", u32},
                                       {"_gridPad1", u32},
                                       {"clipPolygonPlanes", planesArray},
                                   }));
  const IrType bandType = e(IrType::Struct("Band", {
                                                       {"curveStart", u32},
                                                       {"curveCount", u32},
                                                       {"yMin", f32},
                                                       {"yMax", f32},
                                                       {"xMin", f32},
                                                       {"xMax", f32},
                                                       {"_pad0", f32},
                                                       {"_pad1", f32},
                                                   }));
  const IrType instanceTransformType =
      e(IrType::Struct("InstanceTransform", {{"row0", vec4f}, {"row1", vec4f}}));
  const IrType quadraticType =
      e(IrType::Struct("Quadratic", {{"p0", vec2f}, {"p1", vec2f}, {"p2", vec2f}}));
  const IrType rayCoverageType = e(IrType::Struct("RayCoverage", {{"cov", f32}, {"wgt", f32}}));

  const IrType bandArray = e(IrType::RuntimeArray(bandType));
  const IrType floatArray = e(IrType::RuntimeArray(f32));
  const IrType u32Array = e(IrType::RuntimeArray(u32));
  const IrType instanceArray = e(IrType::RuntimeArray(instanceTransformType));

  // ----- Module scope: constant + the 12 bindings at group 0 -----
  ModuleBuilder builder;
  e.ok(builder.addConstant("kNoBand", U(0xFFFFFFFFu)));
  e.ok(builder.addUniformBuffer(0, 0, "uniforms", uniformsType));
  e.ok(builder.addReadOnlyStorageBuffer(0, 1, "bands", bandArray));
  e.ok(builder.addReadOnlyStorageBuffer(0, 2, "curveData", floatArray));
  e.ok(builder.addTexture2d(0, 3, "patternTexture"));
  e.ok(builder.addSampler(0, 4, "patternSampler"));
  e.ok(builder.addTexture2d(0, 5, "clipMaskTexture"));
  e.ok(builder.addSampler(0, 6, "clipMaskSampler"));
  e.ok(builder.addReadOnlyStorageBuffer(0, 7, "instanceTransforms", instanceArray));
  e.ok(builder.addReadOnlyStorageBuffer(0, 8, "vBands", bandArray));
  e.ok(builder.addReadOnlyStorageBuffer(0, 9, "vCurveData", floatArray));
  e.ok(builder.addReadOnlyStorageBuffer(0, 10, "hBandGrid", u32Array));
  e.ok(builder.addReadOnlyStorageBuffer(0, 11, "vBandGrid", u32Array));

  // ----- clip_mask_coverage(pixel_center) -> f32 -----
  {
    auto result =
        builder.createFunction("clip_mask_coverage", {IrParam{"pixel_center", vec2f}}, f32);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr texture = e(fn.ref("clipMaskTexture"));
    const IrExpr pixelCenter = e(fn.ref("pixel_center"));
    const IrExpr dims = e(fn.addLet(
        "dims",
        e(Convert(IrType::Vec2i(), e(CallBuiltin(BuiltinFn::TextureDimensions, {texture}))))));
    const IrExpr texel = e(fn.addLet(
        "texel", e(CallBuiltin(
                     BuiltinFn::Clamp,
                     {e(Convert(IrType::Vec2i(),
                                e(CallBuiltin(
                                    BuiltinFn::Round,
                                    {e(Sub(pixelCenter, e(ConstructVector(vec2f, {F(0.5f)}))))})))),
                      e(ConstructVector(IrType::Vec2i(), {I(0)})),
                      e(Sub(dims, e(ConstructVector(IrType::Vec2i(), {I(1)}))))}))));
    const IrExpr sample =
        e(fn.addLet("sample", e(CallBuiltin(BuiltinFn::TextureLoad, {texture, texel, I(0)}))));

    // slug_fill's `.r/.g/.b/.a` transliterated to `.x/.y/.z/.w`.
    const IrExpr sum = e(Add(
        e(Add(e(Add(e(Swizzle(sample, "x")), e(Swizzle(sample, "y")))), e(Swizzle(sample, "z")))),
        e(Swizzle(sample, "w"))));
    e.ok(fn.returnValue(
        e(CallBuiltin(BuiltinFn::Clamp, {e(Mul(sum, F(0.25f))), F(0.0f), F(1.0f)}))));
    e.ok(fn.finish());
  }

  // ----- load_h_curve / load_v_curve -----
  BuildCurveLoader(e, builder, quadraticType, "load_h_curve", "curveData");
  BuildCurveLoader(e, builder, quadraticType, "load_v_curve", "vCurveData");

  // ----- solve_quadratic(a, b, c) -> vec2f (Citardauq form) -----
  {
    auto result = builder.createFunction(
        "solve_quadratic", {IrParam{"a", f32}, IrParam{"b", f32}, IrParam{"c", f32}}, vec2f);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr a = e(fn.ref("a"));
    const IrExpr b = e(fn.ref("b"));
    const IrExpr c = e(fn.ref("c"));
    const IrExpr roots =
        e(fn.addVar("roots", vec2f, e(ConstructVector(vec2f, {F(-1.0f), F(-1.0f)}))));

    // Degenerate (nearly linear) curves solve bt + c = 0.
    e.ok(fn.beginIf(e(Lt(e(CallBuiltin(BuiltinFn::Abs, {a})), F(1e-4f)))));
    {
      e.ok(fn.beginIf(e(Gt(e(CallBuiltin(BuiltinFn::Abs, {b})), F(1e-6f)))));
      {
        const IrExpr t = e(fn.addLet("t", e(Div(e(Neg(c)), b))));
        e.ok(fn.beginIf(e(And(e(Ge(t, F(0.0f))), e(Le(t, F(1.0f)))))));
        e.ok(fn.assign(e(Swizzle(roots, "x")), t));
        e.ok(fn.endIf());
      }
      e.ok(fn.endIf());
      e.ok(fn.returnValue(roots));
    }
    e.ok(fn.endIf());

    const IrExpr disc = e(fn.addLet("disc", e(Sub(e(Mul(b, b)), e(Mul(e(Mul(F(4.0f), a)), c))))));
    e.ok(fn.beginIf(e(Lt(disc, F(0.0f)))));
    e.ok(fn.returnValue(roots));
    e.ok(fn.endIf());

    const IrExpr sqrtDisc = e(fn.addLet("sqrt_disc", e(CallBuiltin(BuiltinFn::Sqrt, {disc}))));
    // Citardauq: divide by the larger-magnitude root to avoid catastrophic cancellation.
    const IrExpr q = e(fn.addLet(
        "q", e(Mul(F(-0.5f), e(Add(b, e(CallBuiltin(BuiltinFn::Select, {e(Neg(sqrtDisc)), sqrtDisc,
                                                                        e(Ge(b, F(0.0f)))}))))))));
    const IrExpr t0 = e(fn.addLet("t0", e(Div(q, a))));
    const IrExpr t1 = e(fn.addLet(
        "t1", e(CallBuiltin(BuiltinFn::Select,
                            {e(Div(c, q)), e(Mul(e(Add(e(Neg(b)), sqrtDisc)), e(Div(F(0.5f), a)))),
                             e(Lt(e(CallBuiltin(BuiltinFn::Abs, {q})), F(1e-30f)))}))));

    e.ok(fn.beginIf(e(And(e(Ge(t0, F(0.0f))), e(Le(t0, F(1.0f)))))));
    e.ok(fn.assign(e(Swizzle(roots, "x")), t0));
    e.ok(fn.endIf());
    e.ok(fn.beginIf(e(And(e(Ge(t1, F(0.0f))), e(Le(t1, F(1.0f)))))));
    e.ok(fn.assign(e(Swizzle(roots, "y")), t1));
    e.ok(fn.endIf());
    e.ok(fn.returnValue(roots));
    e.ok(fn.finish());
  }

  // ----- owns_axis_sample(start, end, sample) -> bool -----
  // Direction-independent half-open interval [min, max) ownership of a shared vertex on the
  // monotone axis (scanline winding convention; see slug_fill.wgsl for the full rationale).
  {
    auto result = builder.createFunction(
        "owns_axis_sample", {IrParam{"start", f32}, IrParam{"end", f32}, IrParam{"sample", f32}},
        IrType::Bool());
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr start = e(fn.ref("start"));
    const IrExpr end = e(fn.ref("end"));
    const IrExpr sample = e(fn.ref("sample"));
    const IrExpr lo = e(fn.addLet("lo", e(CallBuiltin(BuiltinFn::Min, {start, end}))));
    const IrExpr hi = e(fn.addLet("hi", e(CallBuiltin(BuiltinFn::Max, {start, end}))));
    e.ok(fn.returnValue(e(And(e(Ge(sample, lo)), e(Lt(sample, hi))))));
    e.ok(fn.finish());
  }

  // ----- accumulateHoriz / accumulateVert -----
  // Horizontal ray: +X at y = sample.y; downward (increasing-Y) crossings wind +1.
  BuildAccumulate(e, builder, rayCoverageType,
                  RayConfig{"accumulateHoriz", "ppemX", "bands", "load_h_curve", "y", "x",
                            /*signWhenPositive=*/1.0f, /*signWhenNegative=*/-1.0f});
  // Vertical ray: +Y at x = sample.x; the sign flips so like-signed coverage blends (see
  // slug_fill.wgsl's accumulateVert comment).
  BuildAccumulate(e, builder, rayCoverageType,
                  RayConfig{"accumulateVert", "ppemY", "vBands", "load_v_curve", "x", "y",
                            /*signWhenPositive=*/-1.0f, /*signWhenNegative=*/1.0f});

  // ----- calc_coverage(h, v) -> f32 -----
  {
    auto result = builder.createFunction(
        "calc_coverage", {IrParam{"h", rayCoverageType}, IrParam{"v", rayCoverageType}}, f32);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr h = e(fn.ref("h"));
    const IrExpr v = e(fn.ref("v"));
    const IrExpr blended = e(fn.addLet(
        "blended",
        e(Div(e(CallBuiltin(BuiltinFn::Abs,
                            {e(Add(e(Mul(e(Member(h, "cov")), e(Member(h, "wgt")))),
                                   e(Mul(e(Member(v, "cov")), e(Member(v, "wgt"))))))})),
              e(CallBuiltin(BuiltinFn::Max, {e(Add(e(Member(h, "wgt")), e(Member(v, "wgt")))),
                                             e(Div(F(1.0f), F(65536.0f)))}))))));
    const IrExpr floorCov = e(fn.addLet(
        "floor_cov",
        e(CallBuiltin(BuiltinFn::Min, {e(CallBuiltin(BuiltinFn::Abs, {e(Member(h, "cov"))})),
                                       e(CallBuiltin(BuiltinFn::Abs, {e(Member(v, "cov"))}))}))));
    e.ok(fn.returnValue(e(CallBuiltin(BuiltinFn::Max, {blended, floorCov}))));
    e.ok(fn.finish());
  }

  // ----- sample_in_clip_polygon(pixel_pos) -> bool -----
  {
    auto result = builder.createFunction("sample_in_clip_polygon", {IrParam{"pixel_pos", vec2f}},
                                         IrType::Bool());
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr uniforms = e(fn.ref("uniforms"));
    const IrExpr pixelPos = e(fn.ref("pixel_pos"));

    e.ok(fn.beginIf(e(Eq(e(Member(uniforms, "hasClipPolygon")), U(0)))));
    e.ok(fn.returnValue(LiteralBool(true)));
    e.ok(fn.endIf());

    const IrExpr i = e(fn.beginFor("i", U(0)));
    e.ok(fn.forCondition(e(Lt(i, U(4)))));
    e.ok(fn.forContinuing(i, e(Add(i, U(1)))));
    {
      const IrExpr plane =
          e(fn.addLet("plane", e(Index(e(Member(uniforms, "clipPolygonPlanes")), i))));
      const IrExpr distance =
          e(Add(e(Add(e(Mul(e(Swizzle(plane, "x")), e(Swizzle(pixelPos, "x")))),
                      e(Mul(e(Swizzle(plane, "y")), e(Swizzle(pixelPos, "y")))))),
                e(Swizzle(plane, "z"))));
      e.ok(fn.beginIf(e(Lt(distance, F(-1e-4f)))));
      e.ok(fn.returnValue(LiteralBool(false)));
      e.ok(fn.endIf());
    }
    e.ok(fn.endFor());

    e.ok(fn.returnValue(LiteralBool(true)));
    e.ok(fn.finish());
  }

  // ----- vs_main: dynamic half-pixel dilation (the key Slug innovation) -----
  {
    auto result = builder.createVertexEntryPoint(
        "vs_main",
        {IrParam{"instance_index", u32, std::nullopt, BuiltinInput::InstanceIndex},
         IrParam{"pos", vec2f, 0}, IrParam{"normal", vec2f, 1}, IrParam{"bandIndex", u32, 2}},
        {IrOutputMember{"clip_pos", vec4f, std::nullopt, BuiltinOutput::Position},
         IrOutputMember{"sample_pos", vec2f, 0}});
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr uniforms = e(fn.ref("uniforms"));
    const IrExpr pos = e(fn.ref("pos"));
    const IrExpr normal = e(fn.ref("normal"));

    const IrExpr xf =
        e(fn.addLet("xf", e(Index(e(fn.ref("instanceTransforms")), e(fn.ref("instance_index"))))));
    const IrExpr row0 = e(Member(xf, "row0"));
    const IrExpr row1 = e(Member(xf, "row1"));

    // Column-major expansion of the 2x3 per-instance affine into a mat4x4.
    const IrExpr instanceMat = e(fn.addLet(
        "instance_mat",
        e(ConstructMat4x4f({e(ConstructVector(vec4f, {e(Swizzle(row0, "x")), e(Swizzle(row1, "x")),
                                                      F(0.0f), F(0.0f)})),
                            e(ConstructVector(vec4f, {e(Swizzle(row0, "y")), e(Swizzle(row1, "y")),
                                                      F(0.0f), F(0.0f)})),
                            e(ConstructVector(vec4f, {F(0.0f), F(0.0f), F(1.0f), F(0.0f)})),
                            e(ConstructVector(vec4f, {e(Swizzle(row0, "z")), e(Swizzle(row1, "z")),
                                                      F(0.0f), F(1.0f)}))}))));
    const IrExpr effectiveMvp =
        e(fn.addLet("effective_mvp", e(Mul(e(Member(uniforms, "mvp")), instanceMat))));

    // Dynamic half-pixel dilation: expand the quad by exactly half a pixel in viewport space.
    const IrExpr worldNormal = e(fn.addLet(
        "world_normal",
        e(Swizzle(e(Mul(effectiveMvp, e(ConstructVector(vec4f, {normal, F(0.0f), F(0.0f)})))),
                  "xy"))));
    const IrExpr viewportNormal = e(fn.addLet(
        "viewport_normal", e(Mul(e(Mul(worldNormal, e(Member(uniforms, "viewport")))), F(0.5f)))));
    const IrExpr viewportLen =
        e(fn.addLet("viewport_len", e(CallBuiltin(BuiltinFn::Length, {viewportNormal}))));
    const IrExpr d = e(
        fn.addLet("d", e(Div(F(1.0f), e(CallBuiltin(BuiltinFn::Max, {viewportLen, F(0.001f)}))))));

    const IrExpr dilated = e(fn.addLet("dilated", e(Add(pos, e(Mul(normal, d))))));

    const IrExpr clipPos =
        e(Mul(effectiveMvp, e(ConstructVector(vec4f, {dilated, F(0.0f), F(1.0f)}))));
    e.ok(fn.returnOutputs({clipPos, dilated}));
    e.ok(fn.finish());
  }

  // ----- fs_main: dual-ray analytic coverage, fill rule, clips, and paint -----
  {
    auto result = builder.createFragmentEntryPoint(
        "fs_main",
        {IrParam{"clip_pos", vec4f, std::nullopt, BuiltinInput::Position},
         IrParam{"sample_pos", vec2f, 0}},
        {IrOutputMember{"color", vec4f, 0}});
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr uniforms = e(fn.ref("uniforms"));
    const IrExpr samplePos = e(fn.ref("sample_pos"));

    const IrExpr pixelCenter =
        e(fn.addLet("pixel_center", e(Swizzle(e(fn.ref("clip_pos")), "xy"))));

    // Path-units per pixel, per axis (sample_pos is linear in viewport position).
    const IrExpr ppem =
        e(fn.addLet("ppem", e(Div(F(1.0f), e(CallBuiltin(BuiltinFn::Fwidth, {samplePos}))))));

    // Horizontal band lookup + ray.
    const IrExpr hCov = e(fn.addVar("hCov", rayCoverageType));
    e.ok(fn.assign(e(Member(hCov, "cov")), F(0.0f)));
    e.ok(fn.assign(e(Member(hCov, "wgt")), F(0.0f)));
    e.ok(fn.beginIf(e(Gt(e(Member(uniforms, "hBandCount")), U(0)))));
    {
      const IrExpr hi = e(fn.addLet(
          "hi",
          e(CallBuiltin(
              BuiltinFn::Clamp,
              {e(Convert(IrType::I32(),
                         e(Div(e(Sub(e(Swizzle(samplePos, "y")), e(Member(uniforms, "yBase")))),
                               e(Member(uniforms, "hStride")))))),
               I(0),
               e(Sub(e(Convert(IrType::I32(), e(Member(uniforms, "hBandCount")))), I(1)))}))));
      const IrExpr slot = e(fn.addLet("slot", e(Index(e(fn.ref("hBandGrid")), hi))));
      e.ok(fn.assign(
          hCov, e(fn.callFunction("accumulateHoriz", {slot, samplePos, e(Swizzle(ppem, "x"))}))));
    }
    e.ok(fn.endIf());

    // Vertical band lookup + ray.
    const IrExpr vCov = e(fn.addVar("vCov", rayCoverageType));
    e.ok(fn.assign(e(Member(vCov, "cov")), F(0.0f)));
    e.ok(fn.assign(e(Member(vCov, "wgt")), F(0.0f)));
    e.ok(fn.beginIf(e(Gt(e(Member(uniforms, "vBandCount")), U(0)))));
    {
      const IrExpr vj = e(fn.addLet(
          "vj",
          e(CallBuiltin(
              BuiltinFn::Clamp,
              {e(Convert(IrType::I32(),
                         e(Div(e(Sub(e(Swizzle(samplePos, "x")), e(Member(uniforms, "xBase")))),
                               e(Member(uniforms, "vStride")))))),
               I(0),
               e(Sub(e(Convert(IrType::I32(), e(Member(uniforms, "vBandCount")))), I(1)))}))));
      const IrExpr slot = e(fn.addLet("slot", e(Index(e(fn.ref("vBandGrid")), vj))));
      e.ok(fn.assign(
          vCov, e(fn.callFunction("accumulateVert", {slot, samplePos, e(Swizzle(ppem, "y"))}))));
    }
    e.ok(fn.endIf());

    const IrExpr coverage =
        e(fn.addVar("coverage", f32, e(fn.callFunction("calc_coverage", {hCov, vCov}))));

    // Fill rule: non-zero clamps the signed winding coverage; even-odd folds the RAW coverage
    // via a triangle wave (a hole has combined coverage of about 2, which the wave maps to 0).
    e.ok(fn.beginIf(e(Eq(e(Member(uniforms, "fillRule")), U(0)))));
    e.ok(fn.assign(coverage, e(CallBuiltin(BuiltinFn::Saturate, {coverage}))));
    e.ok(fn.elseBranch());
    e.ok(fn.assign(
        coverage,
        e(Sub(
            F(1.0f),
            e(CallBuiltin(
                BuiltinFn::Abs,
                {e(Sub(F(1.0f), e(Mul(e(CallBuiltin(BuiltinFn::Fract, {e(Mul(coverage, F(0.5f)))})),
                                      F(2.0f)))))}))))));
    e.ok(fn.endIf());

    // Convex clip-polygon test, in viewport-pixel space.
    e.ok(fn.beginIf(e(Not(e(fn.callFunction("sample_in_clip_polygon", {pixelCenter}))))));
    e.ok(fn.assign(coverage, F(0.0f)));
    e.ok(fn.endIf());

    // Path-clip mask coverage (multiplicative).
    const IrExpr clipCoverage = e(fn.addVar("clipCoverage", f32, F(1.0f)));
    e.ok(fn.beginIf(e(Ne(e(Member(uniforms, "hasClipMask")), U(0)))));
    e.ok(fn.assign(clipCoverage, e(fn.callFunction("clip_mask_coverage", {pixelCenter}))));
    e.ok(fn.endIf());
    e.ok(fn.assign(coverage, e(Mul(coverage, clipCoverage))));

    e.ok(fn.beginIf(e(Le(coverage, F(0.0f)))));
    e.ok(fn.discard());
    e.ok(fn.endIf());

    // Solid paint: uniforms.color is premultiplied; scale all channels by coverage.
    e.ok(fn.beginIf(e(Eq(e(Member(uniforms, "paintMode")), U(0)))));
    e.ok(fn.returnOutputs({e(Mul(e(Member(uniforms, "color")), coverage))}));
    e.ok(fn.endIf());

    // Pattern paint: repeat-tiled sampling in pattern-tile space.
    const IrExpr tileSize = e(Member(uniforms, "tileSize"));
    const IrExpr patternPos = e(fn.addLet(
        "patternPos", e(Swizzle(e(Mul(e(Member(uniforms, "patternFromPath")),
                                      e(ConstructVector(vec4f, {samplePos, F(0.0f), F(1.0f)})))),
                                "xy"))));
    const IrExpr wrapped = e(fn.addLet(
        "wrapped",
        e(ConstructVector(
            vec2f,
            {e(Mul(e(CallBuiltin(BuiltinFn::Fract,
                                 {e(Div(e(Swizzle(patternPos, "x")), e(Swizzle(tileSize, "x"))))})),
                   e(Swizzle(tileSize, "x")))),
             e(Mul(e(CallBuiltin(BuiltinFn::Fract,
                                 {e(Div(e(Swizzle(patternPos, "y")), e(Swizzle(tileSize, "y"))))})),
                   e(Swizzle(tileSize, "y"))))}))));
    const IrExpr uv = e(fn.addLet("uv", e(Div(wrapped, tileSize))));
    const IrExpr sampled =
        e(fn.addVar("sampled", vec4f,
                    e(CallBuiltin(BuiltinFn::TextureSample, {e(fn.ref("patternTexture")),
                                                             e(fn.ref("patternSampler")), uv}))));
    e.ok(fn.assign(sampled,
                   e(Mul(e(Mul(sampled, e(Member(uniforms, "patternOpacity")))), coverage))));
    e.ok(fn.returnOutputs({sampled}));
    e.ok(fn.finish());
  }

  if (e.error) {
    return *e.error;
  }
  return builder.build();
}

}  // namespace donner::gpu::shader::programs
