// Slug mask pipeline: analytic dual-ray coverage at 1 sample/pixel, written
// into an RGBA8Unorm mask texture for use as a clip source by the main fill /
// gradient pipelines (Phase 3b path clipping).
//
// Analytic version of the mask (0041 §8). Single convex bounding fan + dense H/V band
// grids → no band-seam double-count. Each fragment writes its scalar coverage
// into ALL FOUR channels; the pipeline blends with BlendOperation::Max, so
// overlapping clip-path draws union as max(c1, c2) per channel (correct union,
// no double-count). The mask reader (`clip_mask_coverage`) averages the four
// channels → returns the coverage, so the Max-union invariant is preserved with
// no reader change.

// ============================================================================
// Uniforms
// ============================================================================

struct Uniforms {
  mvp: mat4x4f,
  viewport: vec2f,
  fillRule: u32,
  hasClipMask: u32,
  // Band-grid parameters (0041 §8.1). Two vec4-aligned rows.
  yBase: f32,
  hStride: f32,
  hBandCount: u32,
  xBase: f32,
  vStride: f32,
  vBandCount: u32,
  antialias: u32,
  _gridPad1: u32,
  boundingVertexCount: u32,
  _boundingPad0: u32,
  _boundingPad1: u32,
  _boundingPad2: u32,
  boundingVertices: array<vec4f, 4>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

struct Band {
  curveStart: u32,
  curveCount: u32,
};

@group(0) @binding(1) var<storage, read> bands: array<Band>;
@group(0) @binding(2) var<storage, read> curveData: array<f32>;
@group(0) @binding(3) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(4) var clipMaskSampler: sampler;
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

fn conservative_path_aabb_expansion(axes: mat2x2f) -> f32 {
  let max_component = max(max(abs(axes[0].x), abs(axes[0].y)),
                          max(abs(axes[1].x), abs(axes[1].y)));
  if (!(max_component > 0.0 && max_component < 1e30)) {
    return 0.0;
  }
  let scaled_axes = mat2x2f(axes[0] / max_component, axes[1] / max_component);
  let scaled_determinant = abs(axes_determinant(scaled_axes));
  if (!(scaled_determinant > 0.0)) {
    return 0.0;
  }
  let scaled_frobenius =
    sqrt(dot(scaled_axes[0], scaled_axes[0]) + dot(scaled_axes[1], scaled_axes[1]));
  let expansion = 0.7071068 * scaled_frobenius /
                  (max_component * scaled_determinant);
  return select(0.0, expansion, expansion > 0.0 && expansion < 1e30);
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

fn load_path_aabb_vertex(expansion: f32, polygon_index: u32) -> vec2f {
  var path_min = vec2f(1e30, 1e30);
  var path_max = vec2f(-1e30, -1e30);
  for (var i = 0u; i < uniforms.boundingVertexCount; i = i + 1u) {
    let position = load_bounding_vertex(i);
    path_min = min(path_min, position);
    path_max = max(path_max, position);
  }
  let left = polygon_index == 0u || polygon_index == 3u;
  let lower = polygon_index < 2u;
  return vec2f(select(path_max.x + expansion, path_min.x - expansion, left),
               select(path_max.y + expansion, path_min.y - expansion, lower));
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
  let path_aabb_expansion = conservative_path_aabb_expansion(axes);
  let use_path_aabb = !axes_are_well_conditioned(axes) && path_aabb_expansion > 0.0;
  let use_device_aabb = needs_device_aabb_fallback(axes);
  let use_aabb = use_path_aabb || use_device_aabb;
  let effective_count = select(uniforms.boundingVertexCount, 4u, use_aabb);
  let triangle = vertex_index / 3u;
  var polygon_index = 0u;
  if (triangle < effective_count - 2u) {
    polygon_index = fan_polygon_index(vertex_index);
  }
  if (use_path_aabb) {
    return load_path_aabb_vertex(path_aabb_expansion, polygon_index);
  }
  if (use_device_aabb) {
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
// Fragment stage
// ============================================================================

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

  // Intersect with a nested clip mask (nested <clipPath> references).
  if (uniforms.hasClipMask != 0u) {
    coverage = coverage * clip_mask_coverage(pixel_center);
  }

  if (coverage <= 0.0) {
    discard;
  }

  // Write the scalar coverage to all four channels; BlendOperation::Max unions
  // overlapping clip-path draws (0041 §8.3).
  var out: FragOutput;
  out.color = vec4f(coverage, coverage, coverage, coverage);
  return out;
}
