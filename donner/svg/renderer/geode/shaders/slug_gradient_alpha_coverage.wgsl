// Alpha-coverage variant of the Slug gradient-fill shader.
//
// Identical to slug_gradient.wgsl except:
//   * FragOutput has no @builtin(sample_mask) — avoids a hang on Intel Arc
//     (Mesa ANV / Xe KMD) when multiple band quads overlap at a pixel.
//   * Coverage is instead folded into the output color as
//     `color *= popcount(mask) / 4.0`, producing equivalent AA via alpha
//     blending on a single-sample render target.
//
// This shader is selected at pipeline creation time when
// `GeodeDevice::useAlphaCoverageAA()` returns true (Intel + Vulkan).
// See slug_gradient.wgsl for the full algorithm commentary.

// ============================================================================
// Uniforms
// ============================================================================

const kMaxStops: u32 = 16u;
const kGradientLinear: u32 = 0u;
const kGradientRadial: u32 = 1u;

// Sentinel returned by `radial_t` when a pixel lies outside the gradient cone
// (negative discriminant or both roots yield negative radii).  Tiny-skia paints
// these pixels transparent via `Mask2PtConicalDegenerates`; we detect the
// sentinel in `fs_main` and discard the fragment to match.
const kInvalidGradientT: f32 = -1e30;

struct GradientUniforms {
  mvp: mat4x4f,
  viewport: vec2f,
  fillRule: u32,
  spreadMode: u32,
  row0: vec4f,
  row1: vec4f,
  startGrad: vec2f,
  endGrad: vec2f,
  radialCenter: vec2f,
  radialFocal: vec2f,
  radialRadius: f32,
  radialFocalRadius: f32,
  gradientKind: u32,
  stopCount: u32,
  stopColors: array<vec4f, kMaxStops>,
  stopOffsets: array<vec4f, 4u>,
  hasClipPolygon: u32,
  hasClipMask: u32,
  _clipPad1: u32,
  _clipPad2: u32,
  clipPolygonPlanes: array<vec4f, 4>,
};

@group(0) @binding(0) var<uniform> uniforms: GradientUniforms;

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
@group(0) @binding(2) var<storage, read> curveData: array<f32>;
@group(0) @binding(3) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(4) var clipMaskSampler: sampler;

fn clip_mask_coverage(pixel_center: vec2f) -> f32 {
  let dims = vec2i(textureDimensions(clipMaskTexture));
  let texel = clamp(vec2i(round(pixel_center - vec2f(0.5))), vec2i(0), dims - vec2i(1));
  let sample = textureLoad(clipMaskTexture, texel, 0);
  return clamp((sample.r + sample.g + sample.b + sample.a) * 0.25, 0.0, 1.0);
}

// ============================================================================
// Vertex stage (identical to slug_gradient.wgsl)
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
// Quadratic Bézier ray intersection (shared with slug_gradient.wgsl)
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
// Gradient evaluation (identical to slug_gradient.wgsl)
// ============================================================================

fn load_stop_offset(i: u32) -> f32 {
  let vec_index = i / 4u;
  let comp = i % 4u;
  let v = uniforms.stopOffsets[vec_index];
  if (comp == 0u) { return v.x; }
  if (comp == 1u) { return v.y; }
  if (comp == 2u) { return v.z; }
  return v.w;
}

fn apply_spread(t: f32, mode: u32) -> f32 {
  if (mode == 1u) {
    var r = t - 2.0 * floor(t * 0.5);
    if (r > 1.0) {
      r = 2.0 - r;
    }
    return r;
  }
  if (mode == 2u) {
    return t - floor(t);
  }
  return clamp(t, 0.0, 1.0);
}

fn sample_stops(t: f32) -> vec4f {
  let count = uniforms.stopCount;
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

  for (var i: u32 = 1u; i < count; i = i + 1u) {
    let o0 = load_stop_offset(i - 1u);
    let o1 = load_stop_offset(i);
    if (t <= o1) {
      let span = max(o1 - o0, 1e-6);
      let f = (t - o0) / span;
      return mix(uniforms.stopColors[i - 1u], uniforms.stopColors[i], f);
    }
  }
  return uniforms.stopColors[count - 1u];
}

fn gradient_space(path_pos: vec2f) -> vec2f {
  let gx = uniforms.row0.x * path_pos.x + uniforms.row0.y * path_pos.y + uniforms.row0.z;
  let gy = uniforms.row1.x * path_pos.x + uniforms.row1.y * path_pos.y + uniforms.row1.z;
  return vec2f(gx, gy);
}

fn linear_t(gpos: vec2f) -> f32 {
  let axis = uniforms.endGrad - uniforms.startGrad;
  let axisLenSq = max(dot(axis, axis), 1e-12);
  return dot(gpos - uniforms.startGrad, axis) / axisLenSq;
}

fn radial_t(gpos: vec2f) -> f32 {
  let F = uniforms.radialFocal;
  let C = uniforms.radialCenter;
  let Fr = uniforms.radialFocalRadius;
  let R = uniforms.radialRadius;

  let e = gpos - F;
  let d = C - F;
  let Dr = R - Fr;

  let A = dot(d, d) - Dr * Dr;
  let B = dot(e, d) + Fr * Dr;
  let Ce = dot(e, e) - Fr * Fr;

  if (abs(A) < 1e-8) {
    if (abs(B) < 1e-8) {
      return 1.0;
    }
    let linear_t = Ce / (2.0 * B);
    if (Fr + linear_t * Dr < 0.0) {
      return kInvalidGradientT;
    }
    return linear_t;
  }

  let disc = B * B - A * Ce;
  if (disc < 0.0) {
    return kInvalidGradientT;
  }

  let sqrtDisc = sqrt(disc);
  let invA = 1.0 / A;
  let t0 = (B - sqrtDisc) * invA;
  let t1 = (B + sqrtDisc) * invA;

  let r1 = Fr + t1 * Dr;
  if (r1 >= 0.0) {
    return t1;
  }
  let r0 = Fr + t0 * Dr;
  if (r0 >= 0.0) {
    return t0;
  }
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

/// Alpha-coverage fragment output: no @builtin(sample_mask).
/// Coverage is folded into color.a instead.
struct FragOutput {
  @location(0) color: vec4f,
};

@fragment
fn fs_main(in: VertexOutput) -> FragOutput {
  let band = bands[in.bandIndex];

  let pixel_center = in.clip_pos.xy;

  let dx = dpdx(in.sample_pos);
  let dy = dpdy(in.sample_pos);

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

  // Convert the 4-bit sample mask into a fractional coverage value.
  let coverage = f32(countOneBits(mask)) / 4.0;

  var clipCoverage: f32 = 1.0;
  if (uniforms.hasClipMask != 0u) {
    clipCoverage = clip_mask_coverage(pixel_center);
    if (clipCoverage <= 0.0) {
      discard;
    }
  }

  let raw_t = gradient_t(in.sample_pos);

  // Radial gradients with conical focal configurations have regions where no
  // circle in the gradient family passes through the pixel (negative
  // discriminant).  Tiny-skia renders these transparent; we match by
  // discarding the fragment.
  if (raw_t < -1e20) {
    discard;
  }
  let t = apply_spread(raw_t, uniforms.spreadMode);
  let straight = sample_stops(t);

  var out: FragOutput;
  // Premultiply straight-alpha gradient color, then scale by coverage.
  out.color = vec4f(straight.rgb * straight.a, straight.a) * clipCoverage * coverage;
  return out;
}
