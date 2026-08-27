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
  const IrType mat2x2f = IrType::Mat2x2f();
  const IrType mat4x4f = IrType::Mat4x4f();

  // ----- Struct types (byte layouts anchored by the layout tests) -----
  const IrType planesArray = e(IrType::SizedArray(vec4f, 4));
  const IrType uniformsType = e(IrType::Struct("Uniforms", {
                                                               {"mvp", mat4x4f},
                                                               {"patternFromPath", mat4x4f},
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
                                                               {"boundingVertexCount", u32},
                                                               {"_gridPad1", u32},
                                                               {"clipPolygonPlanes", planesArray},
                                                               {"boundingVertices", planesArray},
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

  // ----- Bounding-geometry vertex stage (slug_fill.wgsl's `vs_main` and its helpers) -----
  //
  // The WGSL threads the bounding polygon through a `ShapeParams` value so one set of helpers
  // serves both of its entry points: `vs_main` reads the polygon from the uniform, and
  // `vs_main_batched` reads it from the instance record. This module re-expresses the
  // non-batched pipeline only, so that parameter has exactly one value everywhere and the
  // helpers below read the uniform directly. Declaration order is the IR's (callees first)
  // rather than the WGSL's, which permits forward references. Every floating-point expression
  // is otherwise the WGSL's, in the WGSL's association order, because the MSL emitted from here
  // is compared against pixels the WGSL produced.

  // ----- load_bounding_vertex(index) -> vec2f -----
  {
    auto result = builder.createFunction("load_bounding_vertex", {IrParam{"index", u32}}, vec2f);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr index = e(fn.ref("index"));
    const IrExpr pair = e(fn.addLet(
        "pair",
        e(Index(e(Member(e(fn.ref("uniforms")), "boundingVertices")), e(Div(index, U(2)))))));
    // The WGSL writes `(index & 1u) != 0u`. The IR has no bitwise and; on u32 the remainder is
    // the same value, and integer arithmetic admits no rounding difference.
    e.ok(fn.returnValue(
        e(CallBuiltin(BuiltinFn::Select, {e(Swizzle(pair, "xy")), e(Swizzle(pair, "zw")),
                                          e(Ne(e(Mod(index, U(2))), U(0)))}))));
    e.ok(fn.finish());
  }

  // ----- fan_polygon_index(vertex_index) -> u32 -----
  {
    auto result = builder.createFunction("fan_polygon_index", {IrParam{"vertex_index", u32}}, u32);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr vertexIndex = e(fn.ref("vertex_index"));
    const IrExpr triangle = e(fn.addLet("triangle", e(Div(vertexIndex, U(3)))));
    const IrExpr corner = e(fn.addLet("corner", e(Mod(vertexIndex, U(3)))));
    e.ok(fn.returnValue(
        e(CallBuiltin(BuiltinFn::Select, {e(Add(triangle, corner)), U(0), e(Eq(corner, U(0)))}))));
    e.ok(fn.finish());
  }

  // ----- axes_determinant(axes) -> f32 -----
  {
    auto result = builder.createFunction("axes_determinant", {IrParam{"axes", mat2x2f}}, f32);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr axes = e(fn.ref("axes"));
    const IrExpr col0 = e(Index(axes, U(0)));
    const IrExpr col1 = e(Index(axes, U(1)));
    e.ok(fn.returnValue(e(Sub(e(Mul(e(Swizzle(col0, "x")), e(Swizzle(col1, "y")))),
                              e(Mul(e(Swizzle(col0, "y")), e(Swizzle(col1, "x"))))))));
    e.ok(fn.finish());
  }

  // ----- axes_are_well_conditioned(axes) -> bool -----
  {
    auto result = builder.createFunction("axes_are_well_conditioned", {IrParam{"axes", mat2x2f}},
                                         IrType::Bool());
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr axes = e(fn.ref("axes"));
    const IrExpr axisScale = e(
        fn.addLet("axis_scale", e(Mul(e(CallBuiltin(BuiltinFn::Length, {e(Index(axes, U(0)))})),
                                      e(CallBuiltin(BuiltinFn::Length, {e(Index(axes, U(1)))}))))));
    const IrExpr determinant =
        e(fn.addLet("determinant", e(fn.callFunction("axes_determinant", {axes}))));
    e.ok(fn.returnValue(
        e(And(e(And(e(Gt(axisScale, F(0.0f))), e(Lt(axisScale, F(1e30f))))),
              e(Gt(e(CallBuiltin(BuiltinFn::Abs, {determinant})), e(Mul(axisScale, F(1e-6f)))))))));
    e.ok(fn.finish());
  }

  // ----- path_from_pixel_delta(axes, pixel_delta) -> vec2f -----
  {
    auto result = builder.createFunction(
        "path_from_pixel_delta", {IrParam{"axes", mat2x2f}, IrParam{"pixel_delta", vec2f}}, vec2f);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr axes = e(fn.ref("axes"));
    const IrExpr delta = e(fn.ref("pixel_delta"));
    const IrExpr col0 = e(Index(axes, U(0)));
    const IrExpr col1 = e(Index(axes, U(1)));
    const IrExpr determinant =
        e(fn.addLet("determinant", e(fn.callFunction("axes_determinant", {axes}))));
    const IrExpr x = e(Sub(e(Mul(e(Swizzle(col1, "y")), e(Swizzle(delta, "x")))),
                           e(Mul(e(Swizzle(col1, "x")), e(Swizzle(delta, "y"))))));
    const IrExpr y = e(Add(e(Mul(e(Neg(e(Swizzle(col0, "y")))), e(Swizzle(delta, "x")))),
                           e(Mul(e(Swizzle(col0, "x")), e(Swizzle(delta, "y"))))));
    e.ok(fn.returnValue(e(Div(e(ConstructVector(vec2f, {x, y})), determinant))));
    e.ok(fn.finish());
  }

  // ----- pixel_axes(effective_mvp) -> mat2x2f -----
  {
    auto result =
        builder.createFunction("pixel_axes", {IrParam{"effective_mvp", mat4x4f}}, mat2x2f);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr mvp = e(fn.ref("effective_mvp"));
    const IrExpr viewport = e(Member(e(fn.ref("uniforms")), "viewport"));
    const IrExpr pixelScale = e(
        fn.addLet("pixel_scale",
                  e(ConstructVector(vec2f, {e(Mul(e(Swizzle(viewport, "x")), F(0.5f))),
                                            e(Mul(e(Neg(e(Swizzle(viewport, "y")))), F(0.5f)))}))));
    const IrExpr originPixel = e(fn.addLet(
        "origin_pixel",
        e(Mul(
            e(Swizzle(e(Mul(mvp, e(ConstructVector(vec4f, {F(0.0f), F(0.0f), F(0.0f), F(1.0f)})))),
                      "xy")),
            pixelScale))));
    const IrExpr xAxis = e(fn.addLet(
        "x_axis_pixel",
        e(Sub(e(Mul(e(Swizzle(e(Mul(mvp, e(ConstructVector(vec4f,
                                                           {F(1.0f), F(0.0f), F(0.0f), F(1.0f)})))),
                              "xy")),
                    pixelScale)),
              originPixel))));
    const IrExpr yAxis = e(fn.addLet(
        "y_axis_pixel",
        e(Sub(e(Mul(e(Swizzle(e(Mul(mvp, e(ConstructVector(vec4f,
                                                           {F(0.0f), F(1.0f), F(0.0f), F(1.0f)})))),
                              "xy")),
                    pixelScale)),
              originPixel))));
    e.ok(fn.returnValue(e(ConstructMat2x2f({xAxis, yAxis}))));
    e.ok(fn.finish());
  }

  // ----- conservative_path_aabb_expansion(axes) -> f32 -----
  {
    auto result =
        builder.createFunction("conservative_path_aabb_expansion", {IrParam{"axes", mat2x2f}}, f32);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr axes = e(fn.ref("axes"));
    const IrExpr col0 = e(Index(axes, U(0)));
    const IrExpr col1 = e(Index(axes, U(1)));
    const IrExpr maxComponent = e(fn.addLet(
        "max_component",
        e(CallBuiltin(
            BuiltinFn::Max,
            {e(CallBuiltin(BuiltinFn::Max,
                           {e(CallBuiltin(BuiltinFn::Abs, {e(Swizzle(col0, "x"))})),
                            e(CallBuiltin(BuiltinFn::Abs, {e(Swizzle(col0, "y"))}))})),
             e(CallBuiltin(BuiltinFn::Max,
                           {e(CallBuiltin(BuiltinFn::Abs, {e(Swizzle(col1, "x"))})),
                            e(CallBuiltin(BuiltinFn::Abs, {e(Swizzle(col1, "y"))}))}))}))));

    e.ok(fn.beginIf(e(Not(e(And(e(Gt(maxComponent, F(0.0f))), e(Lt(maxComponent, F(1e30f)))))))));
    e.ok(fn.returnValue(F(0.0f)));
    e.ok(fn.endIf());

    // For A's singular values, sigma_min >= abs(det(A)) / norm_frobenius(A). Expanding the
    // path-space AABB by the radius below therefore makes its transformed image contain the
    // complete half-pixel device-space square. Normalize first so high-shear transforms do not
    // overflow the determinant.
    const IrExpr scaledAxes =
        e(fn.addLet("scaled_axes",
                    e(ConstructMat2x2f({e(Div(col0, maxComponent)), e(Div(col1, maxComponent))}))));
    const IrExpr scaledDeterminant = e(fn.addLet(
        "scaled_determinant",
        e(CallBuiltin(BuiltinFn::Abs, {e(fn.callFunction("axes_determinant", {scaledAxes}))}))));

    e.ok(fn.beginIf(e(Not(e(Gt(scaledDeterminant, F(0.0f)))))));
    e.ok(fn.returnValue(F(0.0f)));
    e.ok(fn.endIf());

    const IrExpr scaledCol0 = e(Index(scaledAxes, U(0)));
    const IrExpr scaledCol1 = e(Index(scaledAxes, U(1)));
    const IrExpr scaledFrobenius = e(fn.addLet(
        "scaled_frobenius",
        e(CallBuiltin(BuiltinFn::Sqrt,
                      {e(Add(e(CallBuiltin(BuiltinFn::Dot, {scaledCol0, scaledCol0})),
                             e(CallBuiltin(BuiltinFn::Dot, {scaledCol1, scaledCol1}))))}))));
    const IrExpr expansion = e(fn.addLet(
        "expansion",
        e(Div(e(Mul(F(0.7071068f), scaledFrobenius)), e(Mul(maxComponent, scaledDeterminant))))));
    e.ok(fn.returnValue(e(CallBuiltin(
        BuiltinFn::Select,
        {F(0.0f), expansion, e(And(e(Gt(expansion, F(0.0f))), e(Lt(expansion, F(1e30f)))))}))));
    e.ok(fn.finish());
  }

  // ----- needs_device_aabb_fallback(axes) -> bool -----
  {
    auto result = builder.createFunction("needs_device_aabb_fallback", {IrParam{"axes", mat2x2f}},
                                         IrType::Bool());
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr axes = e(fn.ref("axes"));
    const IrExpr count = e(Member(e(fn.ref("uniforms")), "boundingVertexCount"));

    e.ok(fn.beginIf(e(Not(e(fn.callFunction("axes_are_well_conditioned", {axes}))))));
    e.ok(fn.returnValue(LiteralBool(false)));
    e.ok(fn.endIf());

    const IrExpr orientation = e(fn.addLet(
        "orientation",
        e(CallBuiltin(
            BuiltinFn::Select,
            {F(-1.0f), F(1.0f), e(Gt(e(fn.callFunction("axes_determinant", {axes})), F(0.0f)))}))));

    const IrExpr i = e(fn.beginFor("i", U(0)));
    e.ok(fn.forCondition(e(Lt(i, count))));
    e.ok(fn.forContinuing(i, e(Add(i, U(1)))));
    {
      const IrExpr previous = e(fn.addLet(
          "previous", e(fn.callFunction("load_bounding_vertex",
                                        {e(Mod(e(Sub(e(Add(i, count)), U(1))), count))}))));
      const IrExpr position =
          e(fn.addLet("position", e(fn.callFunction("load_bounding_vertex", {i}))));
      const IrExpr next = e(fn.addLet(
          "next", e(fn.callFunction("load_bounding_vertex", {e(Mod(e(Add(i, U(1))), count))}))));
      const IrExpr incoming = e(fn.addLet("incoming", e(Mul(axes, e(Sub(position, previous))))));
      const IrExpr outgoing = e(fn.addLet("outgoing", e(Mul(axes, e(Sub(next, position))))));
      const IrExpr incomingLength =
          e(fn.addLet("incoming_length", e(CallBuiltin(BuiltinFn::Length, {incoming}))));
      const IrExpr outgoingLength =
          e(fn.addLet("outgoing_length", e(CallBuiltin(BuiltinFn::Length, {outgoing}))));

      e.ok(fn.beginIf(e(
          Not(e(And(e(And(e(And(e(Gt(incomingLength, F(1e-6f))), e(Lt(incomingLength, F(1e30f))))),
                          e(Gt(outgoingLength, F(1e-6f))))),
                    e(Lt(outgoingLength, F(1e30f)))))))));
      e.ok(fn.returnValue(LiteralBool(true)));
      e.ok(fn.endIf());

      const IrExpr incomingEdge = e(fn.addLet("incoming_edge", e(Div(incoming, incomingLength))));
      const IrExpr outgoingEdge = e(fn.addLet("outgoing_edge", e(Div(outgoing, outgoingLength))));
      const IrExpr incomingNormal = e(fn.addLet(
          "incoming_normal",
          e(Mul(orientation, e(ConstructVector(vec2f, {e(Swizzle(incomingEdge, "y")),
                                                       e(Neg(e(Swizzle(incomingEdge, "x"))))}))))));
      const IrExpr outgoingNormal = e(fn.addLet(
          "outgoing_normal",
          e(Mul(orientation, e(ConstructVector(vec2f, {e(Swizzle(outgoingEdge, "y")),
                                                       e(Neg(e(Swizzle(outgoingEdge, "x"))))}))))));
      const IrExpr denominator = e(fn.addLet(
          "denominator",
          e(Add(F(1.0f), e(CallBuiltin(BuiltinFn::Dot, {incomingNormal, outgoingNormal}))))));

      e.ok(fn.beginIf(e(Not(e(Gt(denominator, F(1e-6f)))))));
      e.ok(fn.returnValue(LiteralBool(true)));
      e.ok(fn.endIf());

      const IrExpr miter = e(fn.addLet(
          "miter", e(Div(e(Mul(F(0.5f), e(Add(incomingNormal, outgoingNormal)))), denominator))));

      e.ok(fn.beginIf(e(Not(e(Le(e(CallBuiltin(BuiltinFn::Length, {miter})), F(2.0f)))))));
      e.ok(fn.returnValue(LiteralBool(true)));
      e.ok(fn.endIf());
    }
    e.ok(fn.endFor());

    e.ok(fn.returnValue(LiteralBool(false)));
    e.ok(fn.finish());
  }

  // ----- load_device_aabb_vertex(effective_mvp, axes, polygon_index) -> vec2f -----
  {
    auto result = builder.createFunction("load_device_aabb_vertex",
                                         {IrParam{"effective_mvp", mat4x4f},
                                          IrParam{"axes", mat2x2f}, IrParam{"polygon_index", u32}},
                                         vec2f);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr mvp = e(fn.ref("effective_mvp"));
    const IrExpr axes = e(fn.ref("axes"));
    const IrExpr polygonIndex = e(fn.ref("polygon_index"));
    const IrExpr uniforms = e(fn.ref("uniforms"));
    const IrExpr viewport = e(Member(uniforms, "viewport"));

    const IrExpr pixelScale = e(
        fn.addLet("pixel_scale",
                  e(ConstructVector(vec2f, {e(Mul(e(Swizzle(viewport, "x")), F(0.5f))),
                                            e(Mul(e(Neg(e(Swizzle(viewport, "y")))), F(0.5f)))}))));
    const IrExpr originPixel = e(fn.addLet(
        "origin_pixel",
        e(Mul(
            e(Swizzle(e(Mul(mvp, e(ConstructVector(vec4f, {F(0.0f), F(0.0f), F(0.0f), F(1.0f)})))),
                      "xy")),
            pixelScale))));
    const IrExpr pixelMin =
        e(fn.addVar("pixel_min", vec2f, e(ConstructVector(vec2f, {F(1e30f), F(1e30f)}))));
    const IrExpr pixelMax =
        e(fn.addVar("pixel_max", vec2f, e(ConstructVector(vec2f, {F(-1e30f), F(-1e30f)}))));

    const IrExpr i = e(fn.beginFor("i", U(0)));
    e.ok(fn.forCondition(e(Lt(i, e(Member(uniforms, "boundingVertexCount"))))));
    e.ok(fn.forContinuing(i, e(Add(i, U(1)))));
    {
      const IrExpr pixel = e(fn.addLet(
          "pixel",
          e(Add(originPixel, e(Mul(axes, e(fn.callFunction("load_bounding_vertex", {i}))))))));
      e.ok(fn.assign(pixelMin, e(CallBuiltin(BuiltinFn::Min, {pixelMin, pixel}))));
      e.ok(fn.assign(pixelMax, e(CallBuiltin(BuiltinFn::Max, {pixelMax, pixel}))));
    }
    e.ok(fn.endFor());

    const IrExpr left =
        e(fn.addLet("left", e(Or(e(Eq(polygonIndex, U(0))), e(Eq(polygonIndex, U(3)))))));
    const IrExpr top = e(fn.addLet("top", e(Lt(polygonIndex, U(2)))));
    const IrExpr pixelCorner = e(fn.addLet(
        "pixel_corner",
        e(ConstructVector(
            vec2f,
            {e(CallBuiltin(BuiltinFn::Select, {e(Add(e(Swizzle(pixelMax, "x")), F(0.5f))),
                                               e(Sub(e(Swizzle(pixelMin, "x")), F(0.5f))), left})),
             e(CallBuiltin(BuiltinFn::Select,
                           {e(Add(e(Swizzle(pixelMax, "y")), F(0.5f))),
                            e(Sub(e(Swizzle(pixelMin, "y")), F(0.5f))), top}))}))));
    e.ok(fn.returnValue(
        e(fn.callFunction("path_from_pixel_delta", {axes, e(Sub(pixelCorner, originPixel))}))));
    e.ok(fn.finish());
  }

  // ----- load_path_aabb_vertex(expansion, polygon_index) -> vec2f -----
  {
    auto result = builder.createFunction(
        "load_path_aabb_vertex", {IrParam{"expansion", f32}, IrParam{"polygon_index", u32}}, vec2f);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr expansion = e(fn.ref("expansion"));
    const IrExpr polygonIndex = e(fn.ref("polygon_index"));
    const IrExpr uniforms = e(fn.ref("uniforms"));

    const IrExpr pathMin =
        e(fn.addVar("path_min", vec2f, e(ConstructVector(vec2f, {F(1e30f), F(1e30f)}))));
    const IrExpr pathMax =
        e(fn.addVar("path_max", vec2f, e(ConstructVector(vec2f, {F(-1e30f), F(-1e30f)}))));

    const IrExpr i = e(fn.beginFor("i", U(0)));
    e.ok(fn.forCondition(e(Lt(i, e(Member(uniforms, "boundingVertexCount"))))));
    e.ok(fn.forContinuing(i, e(Add(i, U(1)))));
    {
      const IrExpr position =
          e(fn.addLet("position", e(fn.callFunction("load_bounding_vertex", {i}))));
      e.ok(fn.assign(pathMin, e(CallBuiltin(BuiltinFn::Min, {pathMin, position}))));
      e.ok(fn.assign(pathMax, e(CallBuiltin(BuiltinFn::Max, {pathMax, position}))));
    }
    e.ok(fn.endFor());

    const IrExpr left =
        e(fn.addLet("left", e(Or(e(Eq(polygonIndex, U(0))), e(Eq(polygonIndex, U(3)))))));
    const IrExpr lower = e(fn.addLet("lower", e(Lt(polygonIndex, U(2)))));
    e.ok(fn.returnValue(e(ConstructVector(
        vec2f,
        {e(CallBuiltin(BuiltinFn::Select, {e(Add(e(Swizzle(pathMax, "x")), expansion)),
                                           e(Sub(e(Swizzle(pathMin, "x")), expansion)), left})),
         e(CallBuiltin(BuiltinFn::Select,
                       {e(Add(e(Swizzle(pathMax, "y")), expansion)),
                        e(Sub(e(Swizzle(pathMin, "y")), expansion)), lower}))}))));
    e.ok(fn.finish());
  }

  // ----- dilated_bounding_vertex(axes, polygon_index) -> vec2f -----
  {
    auto result =
        builder.createFunction("dilated_bounding_vertex",
                               {IrParam{"axes", mat2x2f}, IrParam{"polygon_index", u32}}, vec2f);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr axes = e(fn.ref("axes"));
    const IrExpr polygonIndex = e(fn.ref("polygon_index"));

    const IrExpr count =
        e(fn.addLet("count", e(Member(e(fn.ref("uniforms")), "boundingVertexCount"))));
    const IrExpr previousIndex =
        e(fn.addLet("previous_index", e(Mod(e(Sub(e(Add(polygonIndex, count)), U(1))), count))));
    const IrExpr nextIndex = e(fn.addLet("next_index", e(Mod(e(Add(polygonIndex, U(1))), count))));
    const IrExpr previous =
        e(fn.addLet("previous", e(fn.callFunction("load_bounding_vertex", {previousIndex}))));
    const IrExpr position =
        e(fn.addLet("position", e(fn.callFunction("load_bounding_vertex", {polygonIndex}))));
    const IrExpr next =
        e(fn.addLet("next", e(fn.callFunction("load_bounding_vertex", {nextIndex}))));

    // Work in viewport pixels, including WebGPU's Y flip. Intersect the two adjacent edge
    // half-planes after moving each outward by half a pixel, then map that miter back to path
    // space so the fragment shader's analytic sample coordinates remain exact.
    e.ok(fn.beginIf(e(Not(e(fn.callFunction("axes_are_well_conditioned", {axes}))))));
    e.ok(fn.returnValue(position));
    e.ok(fn.endIf());

    const IrExpr previousEdge = e(fn.addLet(
        "previous_edge",
        e(CallBuiltin(BuiltinFn::Normalize, {e(Mul(axes, e(Sub(position, previous))))}))));
    const IrExpr nextEdge = e(fn.addLet(
        "next_edge", e(CallBuiltin(BuiltinFn::Normalize, {e(Mul(axes, e(Sub(next, position))))}))));
    const IrExpr orientation = e(fn.addLet(
        "orientation",
        e(CallBuiltin(
            BuiltinFn::Select,
            {F(-1.0f), F(1.0f), e(Gt(e(fn.callFunction("axes_determinant", {axes})), F(0.0f)))}))));
    const IrExpr previousNormal = e(fn.addLet(
        "previous_normal",
        e(Mul(orientation, e(ConstructVector(vec2f, {e(Swizzle(previousEdge, "y")),
                                                     e(Neg(e(Swizzle(previousEdge, "x"))))}))))));
    const IrExpr nextNormal = e(fn.addLet(
        "next_normal",
        e(Mul(orientation, e(ConstructVector(vec2f, {e(Swizzle(nextEdge, "y")),
                                                     e(Neg(e(Swizzle(nextEdge, "x"))))}))))));
    const IrExpr miterDenominator =
        e(fn.addLet("miter_denominator",
                    e(Add(F(1.0f), e(CallBuiltin(BuiltinFn::Dot, {previousNormal, nextNormal}))))));
    const IrExpr pixelDelta =
        e(fn.addLet("pixel_delta",
                    e(Div(e(Mul(F(0.5f), e(Add(previousNormal, nextNormal)))), miterDenominator))));
    e.ok(fn.returnValue(
        e(Add(position, e(fn.callFunction("path_from_pixel_delta", {axes, pixelDelta}))))));
    e.ok(fn.finish());
  }

  // ----- effective_bounding_vertex(effective_mvp, vertex_index) -> vec2f -----
  {
    auto result = builder.createFunction(
        "effective_bounding_vertex",
        {IrParam{"effective_mvp", mat4x4f}, IrParam{"vertex_index", u32}}, vec2f);
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr mvp = e(fn.ref("effective_mvp"));
    const IrExpr vertexIndex = e(fn.ref("vertex_index"));

    const IrExpr axes = e(fn.addLet("axes", e(fn.callFunction("pixel_axes", {mvp}))));
    const IrExpr pathAabbExpansion = e(fn.addLet(
        "path_aabb_expansion", e(fn.callFunction("conservative_path_aabb_expansion", {axes}))));
    const IrExpr usePathAabb = e(fn.addLet(
        "use_path_aabb", e(And(e(Not(e(fn.callFunction("axes_are_well_conditioned", {axes})))),
                               e(Gt(pathAabbExpansion, F(0.0f)))))));
    const IrExpr useDeviceAabb =
        e(fn.addLet("use_device_aabb", e(fn.callFunction("needs_device_aabb_fallback", {axes}))));
    const IrExpr useAabb = e(fn.addLet("use_aabb", e(Or(usePathAabb, useDeviceAabb))));
    const IrExpr effectiveCount = e(fn.addLet(
        "effective_count",
        e(CallBuiltin(BuiltinFn::Select,
                      {e(Member(e(fn.ref("uniforms")), "boundingVertexCount")), U(4), useAabb}))));
    const IrExpr triangle = e(fn.addLet("triangle", e(Div(vertexIndex, U(3)))));
    const IrExpr polygonIndex = e(fn.addVar("polygon_index", u32, U(0)));

    e.ok(fn.beginIf(e(Lt(triangle, e(Sub(effectiveCount, U(2)))))));
    e.ok(fn.assign(polygonIndex, e(fn.callFunction("fan_polygon_index", {vertexIndex}))));
    e.ok(fn.endIf());

    e.ok(fn.beginIf(usePathAabb));
    e.ok(fn.returnValue(
        e(fn.callFunction("load_path_aabb_vertex", {pathAabbExpansion, polygonIndex}))));
    e.ok(fn.endIf());

    e.ok(fn.beginIf(useDeviceAabb));
    e.ok(fn.returnValue(e(fn.callFunction("load_device_aabb_vertex", {mvp, axes, polygonIndex}))));
    e.ok(fn.endIf());

    e.ok(fn.returnValue(e(fn.callFunction("dilated_bounding_vertex", {axes, polygonIndex}))));
    e.ok(fn.finish());
  }

  // ----- vs_main: the convex bounding fan, dilated half a pixel per edge -----
  {
    auto result = builder.createVertexEntryPoint(
        "vs_main",
        {IrParam{"vertex_index", u32, std::nullopt, BuiltinInput::VertexIndex},
         IrParam{"instance_index", u32, std::nullopt, BuiltinInput::InstanceIndex}},
        {IrOutputMember{"clip_pos", vec4f, std::nullopt, BuiltinOutput::Position},
         IrOutputMember{"sample_pos", vec2f, 0}});
    if (result.hasError()) {
      return std::move(result).error();
    }
    FunctionBuilder fn = std::move(result).result();

    const IrExpr uniforms = e(fn.ref("uniforms"));

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

    const IrExpr dilated =
        e(fn.addLet("dilated", e(fn.callFunction("effective_bounding_vertex",
                                                 {effectiveMvp, e(fn.ref("vertex_index"))}))));

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
