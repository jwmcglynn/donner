// Slug gradient-fill: analytic dual-ray coverage at 1 sample/pixel.
//
// Parallel to slug_fill.wgsl (see that file for the full analytic-AA
// commentary, 0041 §1/§8) but the fragment evaluates a linear/radial gradient
// at the pixel center instead of a solid color, then folds the analytic
// coverage into the premultiplied output. Single convex bounding fan + dense H/V
// band grids → no band-seam double-count.

// ============================================================================
// Uniforms
// ============================================================================

const kMaxStops: u32 = 16u;
const kGradientLinear: u32 = 0u;
const kGradientRadial: u32 = 1u;

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
  antialias: u32,
  _clipPad2: u32,
  // Band-grid parameters (0041 §8.1). Two vec4-aligned rows.
  yBase: f32,
  hStride: f32,
  hBandCount: u32,
  xBase: f32,
  vStride: f32,
  vBandCount: u32,
  _gridPad0: u32,
  _gridPad1: u32,
  clipPolygonPlanes: array<vec4f, 4>,
  boundingVertexCount: u32,
  _boundingPad0: u32,
  _boundingPad1: u32,
  _boundingPad2: u32,
  boundingVertices: array<vec4f, 4>,
};

@group(0) @binding(0) var<uniform> uniforms: GradientUniforms;

struct Band {
  curveStart: u32,
  curveCount: u32,
};

@group(0) @binding(1) var<storage, read> bands: array<Band>;
@group(0) @binding(2) var<storage, read> curveData: array<f32>;
@group(0) @binding(3) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(4) var clipMaskSampler: sampler;
// Vertical bands/curves + dense band grids (analytic dual-ray, 0041 §8).
@group(0) @binding(5) var<storage, read> vBands: array<Band>;
@group(0) @binding(6) var<storage, read> vCurveData: array<f32>;
@group(0) @binding(7) var<storage, read> hBandGrid: array<u32>;
@group(0) @binding(8) var<storage, read> vBandGrid: array<u32>;
@group(0) @binding(9) var<storage, read> hCurveIndices: array<u32>;
@group(0) @binding(10) var<storage, read> vCurveIndices: array<u32>;

const kNoBand: u32 = 0xFFFFFFFFu;

fn clip_mask_coverage(pixel_center: vec2f) -> f32 {
  let dims = vec2i(textureDimensions(clipMaskTexture));
  let texel = clamp(vec2i(round(pixel_center - vec2f(0.5))), vec2i(0), dims - vec2i(1));
  let sample = textureLoad(clipMaskTexture, texel, 0);
  return clamp((sample.r + sample.g + sample.b + sample.a) * 0.25, 0.0, 1.0);
}

// ============================================================================
// Vertex stage
// ============================================================================

struct VertexOutput {
  @builtin(position) clip_pos: vec4f,
  @location(0) sample_pos: vec2f,
};

fn load_bounding_vertex(index: u32) -> vec2f {
  let pair = uniforms.boundingVertices[index / 2u];
  return select(pair.xy, pair.zw, (index & 1u) != 0u);
}

fn fan_polygon_index(vertex_index: u32) -> u32 {
  let triangle = vertex_index / 3u;
  let corner = vertex_index % 3u;
  return select(triangle + corner, 0u, corner == 0u);
}

fn pixel_axes(effective_mvp: mat4x4f) -> mat2x2f {
  let pixel_scale = vec2f(uniforms.viewport.x * 0.5, -uniforms.viewport.y * 0.5);
  let origin_pixel = (effective_mvp * vec4f(0.0, 0.0, 0.0, 1.0)).xy * pixel_scale;
  let x_axis_pixel =
    (effective_mvp * vec4f(1.0, 0.0, 0.0, 1.0)).xy * pixel_scale - origin_pixel;
  let y_axis_pixel =
    (effective_mvp * vec4f(0.0, 1.0, 0.0, 1.0)).xy * pixel_scale - origin_pixel;
  return mat2x2f(x_axis_pixel, y_axis_pixel);
}

fn axes_determinant(axes: mat2x2f) -> f32 {
  return axes[0].x * axes[1].y - axes[0].y * axes[1].x;
}

fn axes_are_well_conditioned(axes: mat2x2f) -> bool {
  let axis_scale = length(axes[0]) * length(axes[1]);
  let determinant = axes_determinant(axes);
  return axis_scale > 0.0 && axis_scale < 1e30 &&
         abs(determinant) > axis_scale * 1e-6;
}

fn path_from_pixel_delta(axes: mat2x2f, pixel_delta: vec2f) -> vec2f {
  let determinant = axes_determinant(axes);
  return vec2f(axes[1].y * pixel_delta.x - axes[1].x * pixel_delta.y,
               -axes[0].y * pixel_delta.x + axes[0].x * pixel_delta.y) /
         determinant;
}

fn needs_device_aabb_fallback(axes: mat2x2f) -> bool {
  if (!axes_are_well_conditioned(axes)) {
    return false;
  }
  let orientation = select(-1.0, 1.0, axes_determinant(axes) > 0.0);
  for (var i = 0u; i < uniforms.boundingVertexCount; i = i + 1u) {
    let previous = load_bounding_vertex(
      (i + uniforms.boundingVertexCount - 1u) % uniforms.boundingVertexCount);
    let position = load_bounding_vertex(i);
    let next = load_bounding_vertex((i + 1u) % uniforms.boundingVertexCount);
    let incoming = axes * (position - previous);
    let outgoing = axes * (next - position);
    let incoming_length = length(incoming);
    let outgoing_length = length(outgoing);
    if (!(incoming_length > 1e-6 && incoming_length < 1e30 &&
          outgoing_length > 1e-6 && outgoing_length < 1e30)) {
      return true;
    }
    let incoming_edge = incoming / incoming_length;
    let outgoing_edge = outgoing / outgoing_length;
    let incoming_normal = orientation * vec2f(incoming_edge.y, -incoming_edge.x);
    let outgoing_normal = orientation * vec2f(outgoing_edge.y, -outgoing_edge.x);
    let denominator = 1.0 + dot(incoming_normal, outgoing_normal);
    if (!(denominator > 1e-6)) {
      return true;
    }
    let miter = 0.5 * (incoming_normal + outgoing_normal) / denominator;
    if (!(length(miter) <= 2.0)) {
      return true;
    }
  }
  return false;
}

fn load_device_aabb_vertex(effective_mvp: mat4x4f, axes: mat2x2f,
                           polygon_index: u32) -> vec2f {
  let pixel_scale = vec2f(uniforms.viewport.x * 0.5, -uniforms.viewport.y * 0.5);
  let origin_pixel = (effective_mvp * vec4f(0.0, 0.0, 0.0, 1.0)).xy * pixel_scale;
  var pixel_min = vec2f(1e30, 1e30);
  var pixel_max = vec2f(-1e30, -1e30);
  for (var i = 0u; i < uniforms.boundingVertexCount; i = i + 1u) {
    let pixel = origin_pixel + axes * load_bounding_vertex(i);
    pixel_min = min(pixel_min, pixel);
    pixel_max = max(pixel_max, pixel);
  }
  let left = polygon_index == 0u || polygon_index == 3u;
  let top = polygon_index < 2u;
  let pixel_corner = vec2f(select(pixel_max.x + 0.5, pixel_min.x - 0.5, left),
                           select(pixel_max.y + 0.5, pixel_min.y - 0.5, top));
  return path_from_pixel_delta(axes, pixel_corner - origin_pixel);
}

fn dilated_bounding_vertex(axes: mat2x2f, polygon_index: u32) -> vec2f {
  let count = uniforms.boundingVertexCount;
  let previous = load_bounding_vertex((polygon_index + count - 1u) % count);
  let position = load_bounding_vertex(polygon_index);
  let next = load_bounding_vertex((polygon_index + 1u) % count);
  if (!axes_are_well_conditioned(axes)) {
    return position;
  }
  let previous_edge = normalize(axes * (position - previous));
  let next_edge = normalize(axes * (next - position));
  let orientation = select(-1.0, 1.0, axes_determinant(axes) > 0.0);
  let previous_normal = orientation * vec2f(previous_edge.y, -previous_edge.x);
  let next_normal = orientation * vec2f(next_edge.y, -next_edge.x);
  let miter_denominator = 1.0 + dot(previous_normal, next_normal);
  let pixel_delta = 0.5 * (previous_normal + next_normal) / miter_denominator;
  return position + path_from_pixel_delta(axes, pixel_delta);
}

fn effective_bounding_vertex(effective_mvp: mat4x4f, vertex_index: u32) -> vec2f {
  let axes = pixel_axes(effective_mvp);
  let use_aabb = needs_device_aabb_fallback(axes);
  let effective_count = select(uniforms.boundingVertexCount, 4u, use_aabb);
  let triangle = vertex_index / 3u;
  var polygon_index = 0u;
  if (triangle < effective_count - 2u) {
    polygon_index = fan_polygon_index(vertex_index);
  }
  if (use_aabb) {
    return load_device_aabb_vertex(effective_mvp, axes, polygon_index);
  }
  return dilated_bounding_vertex(axes, polygon_index);
}

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VertexOutput {
  let dilated = effective_bounding_vertex(uniforms.mvp, vertex_index);

  var out: VertexOutput;
  out.clip_pos = uniforms.mvp * vec4f(dilated, 0.0, 1.0);
  out.sample_pos = dilated;
  return out;
}

// ============================================================================
// Quadratic root solving + analytic per-ray coverage (mirrors slug_fill.wgsl)
// ============================================================================

struct Quadratic {
  p0: vec2f,
  p1: vec2f,
  p2: vec2f,
};

fn load_h_curve(index: u32) -> Quadratic {
  let base = index * 6u;
  var q: Quadratic;
  q.p0 = vec2f(curveData[base + 0u], curveData[base + 1u]);
  q.p1 = vec2f(curveData[base + 2u], curveData[base + 3u]);
  q.p2 = vec2f(curveData[base + 4u], curveData[base + 5u]);
  return q;
}

fn load_v_curve(index: u32) -> Quadratic {
  let base = index * 6u;
  var q: Quadratic;
  q.p0 = vec2f(vCurveData[base + 0u], vCurveData[base + 1u]);
  q.p1 = vec2f(vCurveData[base + 2u], vCurveData[base + 3u]);
  q.p2 = vec2f(vCurveData[base + 4u], vCurveData[base + 5u]);
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
  let q = -0.5 * (b + select(-sqrt_disc, sqrt_disc, b >= 0.0));
  let t0 = q / a;
  let t1 = select(c / q, (-b + sqrt_disc) * (0.5 / a), abs(q) < 1e-30);
  if (t0 >= 0.0 && t0 <= 1.0) {
    roots.x = t0;
  }
  if (t1 >= 0.0 && t1 <= 1.0) {
    roots.y = t1;
  }
  return roots;
}

struct RayCoverage {
  cov: f32,
  wgt: f32,
  winding: f32,
};

// Ownership of a shared vertex on the monotone axis, as a direction-independent
// half-open interval [min, max): min-inclusive, max-exclusive. This is the standard
// scanline winding convention. A start-inclusive/end-exclusive test (which flips to
// max-inclusive for a decreasing curve) miscounts a Y-extremum shared vertex as a
// single crossing when the sample lands exactly on the extremum value, breaking fill
// parity for that scanline. Metal never samples exactly on the integer extremum;
// llvmpipe does, so [min, max) is required for cross-backend agreement. Testing the
// monotone axis directly also avoids backend-dependent root rounding near t=1.
fn owns_axis_sample(start: f32, end: f32, sample: f32) -> bool {
  let lo = min(start, end);
  let hi = max(start, end);
  return sample >= lo && sample < hi;
}

fn accumulateHoriz(slot: u32, sample: vec2f, ppemX: f32) -> RayCoverage {
  var result: RayCoverage;
  result.cov = 0.0;
  result.wgt = 0.0;
  result.winding = 0.0;
  if (slot == kNoBand) {
    return result;
  }
  let band = bands[slot];
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_h_curve(hCurveIndices[band.curveStart + i]);
    let curve_max_x = max(curve.p0.x, max(curve.p1.x, curve.p2.x));
    if ((curve_max_x - sample.x) * ppemX <= -0.5) {
      break;
    }
    if (!owns_axis_sample(curve.p0.y, curve.p2.y, sample.y)) {
      continue;
    }
    let a = curve.p0.y - 2.0 * curve.p1.y + curve.p2.y;
    let b = 2.0 * (curve.p1.y - curve.p0.y);
    let c = curve.p0.y - sample.y;
    let roots = solve_quadratic(a, b, c);
    for (var k = 0; k < 2; k = k + 1) {
      let t = select(roots.y, roots.x, k == 0);
      if (t < 0.0) {
        continue;
      }
      let omt = 1.0 - t;
      let x = omt * omt * curve.p0.x + 2.0 * omt * t * curve.p1.x + t * t * curve.p2.x;
      let r = (x - sample.x) * ppemX;
      let dy_dt = 2.0 * omt * (curve.p1.y - curve.p0.y) + 2.0 * t * (curve.p2.y - curve.p1.y);
      let s = select(-1.0, 1.0, dy_dt >= 0.0);
      result.cov = result.cov + s * saturate(r + 0.5);
      result.wgt = max(result.wgt, saturate(1.0 - abs(r) * 2.0));
      result.winding = result.winding + s * select(0.0, 1.0, r >= 0.0);
    }
  }
  return result;
}

fn accumulateVert(slot: u32, sample: vec2f, ppemY: f32) -> RayCoverage {
  var result: RayCoverage;
  result.cov = 0.0;
  result.wgt = 0.0;
  result.winding = 0.0;
  if (slot == kNoBand) {
    return result;
  }
  let band = vBands[slot];
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_v_curve(vCurveIndices[band.curveStart + i]);
    let curve_max_y = max(curve.p0.y, max(curve.p1.y, curve.p2.y));
    if ((curve_max_y - sample.y) * ppemY <= -0.5) {
      break;
    }
    if (!owns_axis_sample(curve.p0.x, curve.p2.x, sample.x)) {
      continue;
    }
    let a = curve.p0.x - 2.0 * curve.p1.x + curve.p2.x;
    let b = 2.0 * (curve.p1.x - curve.p0.x);
    let c = curve.p0.x - sample.x;
    let roots = solve_quadratic(a, b, c);
    for (var k = 0; k < 2; k = k + 1) {
      let t = select(roots.y, roots.x, k == 0);
      if (t < 0.0) {
        continue;
      }
      let omt = 1.0 - t;
      let y = omt * omt * curve.p0.y + 2.0 * omt * t * curve.p1.y + t * t * curve.p2.y;
      let r = (y - sample.y) * ppemY;
      let dx_dt = 2.0 * omt * (curve.p1.x - curve.p0.x) + 2.0 * t * (curve.p2.x - curve.p1.x);
      let s = select(1.0, -1.0, dx_dt >= 0.0);
      result.cov = result.cov + s * saturate(r + 0.5);
      result.wgt = max(result.wgt, saturate(1.0 - abs(r) * 2.0));
      result.winding = result.winding + s * select(0.0, 1.0, r >= 0.0);
    }
  }
  return result;
}

fn calc_coverage(h: RayCoverage, v: RayCoverage) -> f32 {
  let blended = abs(h.cov * h.wgt + v.cov * v.wgt) / max(h.wgt + v.wgt, 1.0 / 65536.0);
  let floor_cov = min(abs(h.cov), abs(v.cov));
  return max(blended, floor_cov);
}

// ============================================================================
// Gradient evaluation
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

struct FragOutput {
  @location(0) color: vec4f,
};

@fragment
fn fs_main(in: VertexOutput) -> FragOutput {
  let pixel_center = in.clip_pos.xy;
  let ppem = 1.0 / fwidth(in.sample_pos);

  var hCov: RayCoverage;
  hCov.cov = 0.0;
  hCov.wgt = 0.0;
  hCov.winding = 0.0;
  if (uniforms.hBandCount > 0u) {
    let hi = clamp(i32((in.sample_pos.y - uniforms.yBase) / uniforms.hStride),
                   0, i32(uniforms.hBandCount) - 1);
    hCov = accumulateHoriz(hBandGrid[hi], in.sample_pos, ppem.x);
  }

  var vCov: RayCoverage;
  vCov.cov = 0.0;
  vCov.wgt = 0.0;
  vCov.winding = 0.0;
  if (uniforms.vBandCount > 0u) {
    let vj = clamp(i32((in.sample_pos.x - uniforms.xBase) / uniforms.vStride),
                   0, i32(uniforms.vBandCount) - 1);
    vCov = accumulateVert(vBandGrid[vj], in.sample_pos, ppem.y);
  }

  var coverage = calc_coverage(hCov, vCov);
  if (uniforms.antialias == 0u) {
    let winding = u32(abs(hCov.winding));
    if (uniforms.fillRule == 0u) {
      coverage = select(0.0, 1.0, winding != 0u);
    } else {
      coverage = f32(winding & 1u);
    }
  } else if (uniforms.fillRule == 0u) {
    coverage = saturate(coverage);
  } else {
    coverage = 1.0 - abs(1.0 - fract(coverage * 0.5) * 2.0);
  }

  if (!sample_in_clip_polygon(pixel_center)) {
    coverage = 0.0;
  }

  var clipCoverage: f32 = 1.0;
  if (uniforms.hasClipMask != 0u) {
    clipCoverage = clip_mask_coverage(pixel_center);
  }
  coverage = coverage * clipCoverage;

  if (coverage <= 0.0) {
    discard;
  }

  let raw_t = gradient_t(in.sample_pos);
  if (raw_t < -1e20) {
    discard;
  }
  let t = apply_spread(raw_t, uniforms.spreadMode);
  let straight = sample_stops(t);

  var out: FragOutput;
  out.color = vec4f(straight.rgb * straight.a, straight.a) * coverage;
  return out;
}
