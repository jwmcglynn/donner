// Slug gradient-fill pipeline: same winding-number coverage evaluation as
// `slug_fill.wgsl`, but the surviving fragments are shaded by a per-pixel
// gradient evaluation instead of a flat uniform color.
//
// Supports linear and radial gradients (Phase 2E/2F). A `gradientKind` field
// in the uniform picks which `t` derivation to run before the shared spread /
// stop-sampling path executes. Sweep (conic) gradients are not supported yet
// because the donner SVG parser does not yet distinguish a <conicGradient>
// element; when that lands, a third branch slots in here the same way.
//
// The pipeline is kept separate from the solid-fill pipeline so that:
//   1. Solid fills keep their lean 128-byte uniform layout with no gradient
//      overhead.
//   2. Gradient kinds share the Slug coverage machinery — the only per-kind
//      cost is the `t` derivation and one more uniform block.
//   3. Changes to the shared Slug coverage code remain mechanically copy-able
//      between files during bring-up. The top half of this file is a straight
//      copy of slug_fill.wgsl's coverage machinery; the fragment stage is where
//      the two diverge.

// ============================================================================
// Uniforms
// ============================================================================

// Maximum number of gradient stops baked into the uniform buffer. This is a
// conservative cap — WebGPU's default uniform-buffer size limit is 64 KiB, so
// we have plenty of headroom. Larger counts should move to a texture lookup
// (TODO: see `GeodeGradientCacheComponent` in the Geode design doc).
const kMaxStops: u32 = 16u;

// gradientKind values.
const kGradientLinear: u32 = 0u;
const kGradientRadial: u32 = 1u;

// Sentinel returned by `radial_t` when a pixel lies outside the gradient cone
// (negative discriminant or both roots yield negative radii).  Tiny-skia paints
// these pixels transparent via `Mask2PtConicalDegenerates`; we detect the
// sentinel in `fs_main` and discard the fragment to match.
const kInvalidGradientT: f32 = -1e30;

struct GradientUniforms {
  // Model-view-projection: path-space → clip space.
  mvp: mat4x4f,
  // Viewport dimensions in pixels.
  viewport: vec2f,
  // 0 = non-zero, 1 = even-odd.
  fillRule: u32,
  // 0 = pad, 1 = reflect, 2 = repeat.
  spreadMode: u32,

  // Affine transform from path-space sample position to gradient space
  // (i.e. `gradientFromPath`). Stored as two row-vectors: the first two
  // components are the 2x2 linear part, the last two are the translation
  // (x then y). WGSL's mat2x3f has awkward alignment so we pack manually.
  // row 0: (a, c, e, _pad) → gx = a*px + c*py + e
  // row 1: (b, d, f, _pad) → gy = b*px + d*py + f
  row0: vec4f,
  row1: vec4f,

  // Linear-gradient: start / end points in gradient space.
  // Unused for radial; still uploaded (zeroed) to keep layout stable.
  startGrad: vec2f,
  endGrad: vec2f,

  // Radial-gradient: center (cx, cy) and focal point (fx, fy) in gradient
  // space, focal radius `fr` and outer radius `r`. Unused for linear.
  radialCenter: vec2f,
  radialFocal: vec2f,
  radialRadius: f32,
  radialFocalRadius: f32,

  // Which gradient-`t` derivation to run (kGradientLinear / kGradientRadial).
  gradientKind: u32,
  // Active number of gradient stops, <= kMaxStops.
  stopCount: u32,

  // Per-stop color (premultiplied alpha).
  stopColors: array<vec4f, kMaxStops>,
  // Per-stop offset in [0, 1]. Packed 4 stops per vec4 to respect WGSL's
  // 16-byte array stride for uniform buffers.
  stopOffsets: array<vec4f, 4u>,

  // Nonzero when a convex 4-vertex clip polygon is active. Same semantics
  // as in `slug_fill.wgsl` — the planes are expressed in viewport-pixel
  // space and tested against each sample's `@builtin(position)` offset.
  hasClipPolygon: u32,
  // Nonzero when a path-clip mask texture is bound at binding 3 (see
  // `slug_fill.wgsl` for the motivation — the gradient pipeline
  // carries its own copy of the flag so neither shader needs to reach
  // into the other's uniform block).
  hasClipMask: u32,
  _clipPad1: u32,
  _clipPad2: u32,
  clipPolygonPlanes: array<vec4f, 4>,
};

@group(0) @binding(0) var<uniform> uniforms: GradientUniforms;

// Per-band metadata. Same layout as slug_fill.wgsl.
struct Band {
  curveStart: u32,
  curveCount: u32,
  yMin: f32,
  yMax: f32,
  xMin: f32,
  xMax: f32,
  _pad0: f32,
  _pad1: f32,
};

@group(0) @binding(1) var<storage, read> bands: array<Band>;

// Flat float array of quadratic Bézier control points (6 floats per curve).
@group(0) @binding(2) var<storage, read> curveData: array<f32>;

// Path-clip mask texture + sampler (Phase 3b). Always bound; a 1x1
// dummy with value 1.0 is bound when `uniforms.hasClipMask == 0` so
// the sample call is always legal. See `slug_fill.wgsl` for the full
// motivation.
@group(0) @binding(3) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(4) var clipMaskSampler: sampler;

fn clip_mask_coverage(pixel_center: vec2f) -> f32 {
  let dims = vec2i(textureDimensions(clipMaskTexture));
  let texel = clamp(vec2i(round(pixel_center - vec2f(0.5))), vec2i(0), dims - vec2i(1));
  let sample = textureLoad(clipMaskTexture, texel, 0);
  return clamp((sample.r + sample.g + sample.b + sample.a) * 0.25, 0.0, 1.0);
}

// ============================================================================
// Vertex stage
// ============================================================================

struct VertexInput {
  @location(0) pos: vec2f,
  @location(1) normal: vec2f,
  @location(2) bandIndex: u32,
};

struct VertexOutput {
  @builtin(position) clip_pos: vec4f,
  @location(0) sample_pos: vec2f,
  @location(1) @interpolate(flat) bandIndex: u32,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
  // Dynamic half-pixel dilation — same as slug_fill.wgsl.
  let world_normal = (uniforms.mvp * vec4f(in.normal, 0.0, 0.0)).xy;
  let viewport_normal = world_normal * uniforms.viewport * 0.5;
  let viewport_len = length(viewport_normal);
  let d = 1.0 / max(viewport_len, 0.001);

  let dilated = in.pos + in.normal * d;

  var out: VertexOutput;
  out.clip_pos = uniforms.mvp * vec4f(dilated, 0.0, 1.0);
  out.sample_pos = dilated;
  out.bandIndex = in.bandIndex;
  return out;
}

// ============================================================================
// Quadratic Bézier ray intersection (shared with slug_fill.wgsl)
// ============================================================================

struct Quadratic {
  p0: vec2f,
  p1: vec2f,
  p2: vec2f,
};

fn load_curve(index: u32) -> Quadratic {
  let base = index * 6u;
  var q: Quadratic;
  q.p0 = vec2f(curveData[base + 0u], curveData[base + 1u]);
  q.p1 = vec2f(curveData[base + 2u], curveData[base + 3u]);
  q.p2 = vec2f(curveData[base + 4u], curveData[base + 5u]);
  return q;
}

fn solve_quadratic(a: f32, b: f32, c: f32) -> vec2f {
  var roots = vec2f(-1.0, -1.0);

  // Degenerate-quadratic threshold. LineTo segments are encoded as degenerate
  // quads with `p1 = midpoint(p0, p2)`, so theoretically `a = p0.y - 2*p1.y
  // + p2.y = 0`. In float32 rounding, `2*p1.y` does not exactly equal
  // `p0.y + p2.y`, and `a` ends up a tiny noise value (empirically up to
  // ~1.5e-5 for the rect2 rotated path). The original `abs(a) < 1e-6`
  // threshold let these noisy degenerates slip into the quadratic branch,
  // where solving with `a ≈ 1e-7` amplifies the noise into spurious root
  // positions — producing scattered single-pixel fill artifacts on
  // stroked closed curves. 1e-4 reliably routes all LineTo-derived
  // degenerate quadratics through the stable linear branch without
  // catching any real quadratic. Match threshold with `slug_fill.wgsl`.
  if (abs(a) < 1e-4) {
    if (abs(b) > 1e-6) {
      let t = -c / b;
      if (t >= 0.0 && t <= 1.0) {
        roots.x = t;
      }
    }
    return roots;
  }

  let disc = b * b - 4.0 * a * c;
  if (disc < 0.0) {
    return roots;
  }

  let sqrt_disc = sqrt(disc);
  let inv_2a = 0.5 / a;
  let t0 = (-b - sqrt_disc) * inv_2a;
  let t1 = (-b + sqrt_disc) * inv_2a;

  if (t0 >= 0.0 && t0 <= 1.0) {
    roots.x = t0;
  }
  if (t1 >= 0.0 && t1 <= 1.0) {
    roots.y = t1;
  }
  return roots;
}

fn curve_winding(curve: Quadratic, sample: vec2f) -> i32 {
  let a = curve.p0.y - 2.0 * curve.p1.y + curve.p2.y;
  let b = 2.0 * (curve.p1.y - curve.p0.y);
  let c = curve.p0.y - sample.y;

  let roots = solve_quadratic(a, b, c);

  var winding: i32 = 0;
  for (var i = 0; i < 2; i = i + 1) {
    let t = select(roots.y, roots.x, i == 0);
    if (t < 0.0) {
      continue;
    }
    let omt = 1.0 - t;
    let x = omt * omt * curve.p0.x + 2.0 * omt * t * curve.p1.x + t * t * curve.p2.x;
    if (x < sample.x) {
      continue;
    }
    let dy_dt = 2.0 * omt * (curve.p1.y - curve.p0.y) + 2.0 * t * (curve.p2.y - curve.p1.y);
    if (dy_dt > 0.0) {
      winding = winding + 1;
    } else if (dy_dt < 0.0) {
      winding = winding - 1;
    }
  }
  return winding;
}

// ============================================================================
// Gradient evaluation
// ============================================================================

// Fetch a stop offset from the packed vec4f array. `i` is the stop index in
// [0, stopCount); the packed layout is (offsets[i/4])[i%4].
fn load_stop_offset(i: u32) -> f32 {
  let vec_index = i / 4u;
  let comp = i % 4u;
  let v = uniforms.stopOffsets[vec_index];
  // WGSL has no dynamic vector component access — unroll.
  if (comp == 0u) { return v.x; }
  if (comp == 1u) { return v.y; }
  if (comp == 2u) { return v.z; }
  return v.w;
}

// Apply the spread mode to raw `t` so the output is in [0, 1] before we sample
// the stop list.
//   0 = pad:     clamp(t, 0, 1)
//   1 = reflect: triangle wave of period 2
//   2 = repeat:  fract(t)
fn apply_spread(t: f32, mode: u32) -> f32 {
  if (mode == 1u) {
    // Reflect: fold into [0, 2), then reflect the second half into [0, 1].
    var r = t - 2.0 * floor(t * 0.5);  // r ∈ [0, 2)
    if (r > 1.0) {
      r = 2.0 - r;
    }
    return r;
  }
  if (mode == 2u) {
    // Repeat: fract() produces a value in [0, 1) even for negative inputs.
    return t - floor(t);
  }
  // Pad.
  return clamp(t, 0.0, 1.0);
}

// Sample the stop list at `t ∈ [0, 1]`. Returns a STRAIGHT-alpha color —
// the caller (`fs_main`) premultiplies before returning to the blend
// pipeline. Stops are uploaded in straight form (see `buildGradientStops`
// + `populateSharedGradientUniforms` in GeoEncoder.cc) so that the mix()
// below linearly interpolates straight RGB/A independently, matching
// tiny-skia / Skia gradient behavior.
fn sample_stops(t: f32) -> vec4f {
  let count = uniforms.stopCount;
  // Degenerate cases.
  if (count == 0u) {
    return vec4f(0.0, 0.0, 0.0, 0.0);
  }
  let firstOffset = load_stop_offset(0u);
  if (t <= firstOffset) {
    return uniforms.stopColors[0];
  }
  let lastOffset = load_stop_offset(count - 1u);
  if (t >= lastOffset) {
    return uniforms.stopColors[count - 1u];
  }

  // Linear search for the straddling stops. Up to kMaxStops iterations; at
  // count=16 this is cheap compared to the curve winding loop.
  for (var i: u32 = 1u; i < count; i = i + 1u) {
    let o0 = load_stop_offset(i - 1u);
    let o1 = load_stop_offset(i);
    if (t <= o1) {
      let span = max(o1 - o0, 1e-6);
      let f = (t - o0) / span;
      return mix(uniforms.stopColors[i - 1u], uniforms.stopColors[i], f);
    }
  }
  // Shouldn't reach here thanks to the t >= lastOffset check above.
  return uniforms.stopColors[count - 1u];
}

// Apply `gradientFromPath` (stored as row0/row1) to a path-space sample.
fn gradient_space(path_pos: vec2f) -> vec2f {
  let gx = uniforms.row0.x * path_pos.x + uniforms.row0.y * path_pos.y + uniforms.row0.z;
  let gy = uniforms.row1.x * path_pos.x + uniforms.row1.y * path_pos.y + uniforms.row1.z;
  return vec2f(gx, gy);
}

// Linear gradient `t`: project `gpos - start` onto `(end - start)`.
fn linear_t(gpos: vec2f) -> f32 {
  let axis = uniforms.endGrad - uniforms.startGrad;
  let axisLenSq = max(dot(axis, axis), 1e-12);
  return dot(gpos - uniforms.startGrad, axis) / axisLenSq;
}

// Two-circle radial gradient `t`, matching SVG 2 / Canvas `createRadialGradient`
// semantics. Given focal point F=(fx,fy) with radius Fr and outer center
// C=(cx,cy) with radius R, a pixel at P is assigned the `t` that satisfies
// `|P - lerp(F, C, t)| = lerp(Fr, R, t)`, i.e.:
//
//     |e - t*d|² = (Fr + t*Dr)²                         with e = P - F,
//                                                            d = C - F,
//                                                            Dr = R - Fr.
//
// Expanding:
//     t²(d·d - Dr²) - 2t(e·d + Fr·Dr) + (e·e - Fr²) = 0.
//
// This is a quadratic in t. We take the root whose corresponding radius
// `Fr + t*Dr` is positive (the classic "outer" root). When `fx==cx` and
// `Fr==0` the quadratic degenerates to `t² = (e·e)/R²` and the whole thing
// collapses to the simple `t = |P-C| / R`.
fn radial_t(gpos: vec2f) -> f32 {
  let F = uniforms.radialFocal;
  let C = uniforms.radialCenter;
  let Fr = uniforms.radialFocalRadius;
  let R = uniforms.radialRadius;

  let e = gpos - F;
  let d = C - F;
  let Dr = R - Fr;

  let A = dot(d, d) - Dr * Dr;
  let B = dot(e, d) + Fr * Dr;  // pre-doubled removed; we use -2B below as per the
                                // standard formula but factor 2 out of B.
  let Ce = dot(e, e) - Fr * Fr;

  // Common case: focal point coincides with center (F == C) and focal radius
  // is zero. Then `d == 0`, `A == -Dr² == -R²`, `B == 0`, and the closed form
  // reduces to `|e|/R`.
  if (abs(A) < 1e-8) {
    // A ~= 0 happens when d·d ≈ Dr², i.e. the "degenerate conic" case. Fall
    // back to the linear closed form to avoid a divide-by-zero.
    if (abs(B) < 1e-8) {
      // Extra-degenerate: F==C and Fr==R — the whole annulus is a single
      // ring. Send everything past the last stop.
      return 1.0;
    }
    let linear_t = Ce / (2.0 * B);
    // Radius must be non-negative for the circle at this `t` to exist.
    if (Fr + linear_t * Dr < 0.0) {
      return kInvalidGradientT;
    }
    return linear_t;
  }

  let disc = B * B - A * Ce;
  if (disc < 0.0) {
    // No real intersection — this pixel is outside the swept cone.  Tiny-skia
    // masks these fragments to transparent; we return a sentinel so `fs_main`
    // can discard.
    return kInvalidGradientT;
  }

  let sqrtDisc = sqrt(disc);
  // Two roots of At² − 2Bt + Ce = 0.
  let invA = 1.0 / A;
  let t0 = (B - sqrtDisc) * invA;
  let t1 = (B + sqrtDisc) * invA;

  // Pick the root whose radius `Fr + t*Dr` is non-negative. Prefer the larger
  // `t` (outer surface), matching Skia's two-point-conical shader.
  let r1 = Fr + t1 * Dr;
  if (r1 >= 0.0) {
    return t1;
  }
  let r0 = Fr + t0 * Dr;
  if (r0 >= 0.0) {
    return t0;
  }
  // Both roots give negative radii — pixel is outside the valid gradient
  // region.  Return the sentinel so the fragment is discarded (transparent).
  return kInvalidGradientT;
}

fn gradient_t(path_pos: vec2f) -> f32 {
  let gpos = gradient_space(path_pos);
  if (uniforms.gradientKind == kGradientRadial) {
    return radial_t(gpos);
  }
  return linear_t(gpos);
}

// ============================================================================
// Fragment stage
// ============================================================================

/// Convex clip-polygon test in viewport-pixel space. Mirrors the
/// identical helper in `slug_fill.wgsl`; the gradient uniform block
/// keeps its own copy of `hasClipPolygon` / `clipPolygonPlanes` so no
/// cross-shader binding is required.
fn sample_in_clip_polygon(pixel_pos: vec2f) -> bool {
  if (uniforms.hasClipPolygon == 0u) {
    return true;
  }
  for (var i = 0u; i < 4u; i = i + 1u) {
    let plane = uniforms.clipPolygonPlanes[i];
    if (plane.x * pixel_pos.x + plane.y * pixel_pos.y + plane.z < -1e-4) {
      return false;
    }
  }
  return true;
}

/// Same shape as `sample_is_inside` in slug_fill.wgsl. Duplicated
/// because WGSL modules are per-shader and the gradient pipeline uses
/// its own uniform buffer layout.
fn sample_is_inside(band: Band, sample_pos: vec2f) -> bool {
  var winding: i32 = 0;
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_curve(band.curveStart + i);
    winding = winding + curve_winding(curve, sample_pos);
  }
  if (uniforms.fillRule == 0u) {
    return winding != 0;
  }
  return (winding & 1) != 0;
}

/// See the FragOutput docstring in slug_fill.wgsl — the gradient path
/// uses the same sample-mask scheme so the 4× MSAA render target
/// receives matched coverage for solid and gradient fills.
struct FragOutput {
  @location(0) color: vec4f,
  @builtin(sample_mask) mask: u32,
};

@fragment
fn fs_main(in: VertexOutput) -> FragOutput {
  let band = bands[in.bandIndex];

  // No pixel-center band-Y clip — see slug_fill.wgsl for the full
  // explanation. Per-sample checks inside the loop own each sample to
  // exactly one band.

  // Framebuffer-pixel center of this fragment, used for the clip
  // polygon test (which is expressed in viewport-pixel space).
  let pixel_center = in.clip_pos.xy;

  let dx = dpdx(in.sample_pos);
  let dy = dpdy(in.sample_pos);

  // Same 4-sample pattern as slug_fill.wgsl.
  var offsets = array<vec2f, 4>(
    vec2f(-0.125, -0.375),
    vec2f( 0.375, -0.125),
    vec2f(-0.375,  0.125),
    vec2f( 0.125,  0.375),
  );

  var mask: u32 = 0u;
  for (var s: u32 = 0u; s < 4u; s = s + 1u) {
    let sp = in.sample_pos + offsets[s].x * dx + offsets[s].y * dy;
    if (sp.y < band.yMin || sp.y >= band.yMax) {
      continue;
    }
    let pixel_sample = pixel_center + offsets[s];
    if (!sample_in_clip_polygon(pixel_sample)) {
      continue;
    }
    if (sample_is_inside(band, sp)) {
      mask = mask | (1u << s);
    }
  }

  if (mask == 0u) {
    discard;
  }

  // Path-clip mask sampling — see the identical block in
  // `slug_fill.wgsl` for the rationale.
  var clipCoverage: f32 = 1.0;
  if (uniforms.hasClipMask != 0u) {
    clipCoverage = clip_mask_coverage(pixel_center);
    if (clipCoverage <= 0.0) {
      discard;
    }
  }

  // Evaluate the gradient stop color at the pixel center. Per-sample
  // gradient sampling would be ~4× the stop search; we intentionally
  // take the cheaper per-pixel path because gradient color varies
  // smoothly and the sample_mask already captures the hard edge AA.
  let raw_t = gradient_t(in.sample_pos);

  // Radial gradients with conical focal configurations have regions where no
  // circle in the gradient family passes through the pixel (negative
  // discriminant).  Tiny-skia renders these transparent; we match by
  // discarding the fragment.
  if (raw_t < -1e20) {
    discard;
  }
  let t = apply_spread(raw_t, uniforms.spreadMode);
  // `sample_stops` returns a STRAIGHT-alpha color (see upload path in
  // GeoEncoder.cc `populateSharedGradientUniforms`). The pipeline blend
  // is premultiplied source-over, so premultiply here before writing.
  let straight = sample_stops(t);

  var out: FragOutput;
  out.color = vec4f(straight.rgb * straight.a, straight.a) * clipCoverage;
  out.mask = mask;
  return out;
}
