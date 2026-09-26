#pragma once
/// @file
/// Authoritative WGSL for SlugGradient.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
/// Authoritative WGSL for SlugGradient.
inline constexpr wgsl::SourceText kSlugGradientSource{
    R"wgsl(// Slug gradient-fill: analytic dual-ray coverage at 1 sample/pixel.
//
// Parallel to SlugFillSource.h (see that file for the full analytic-AA
// commentary) but the fragment evaluates a linear/radial gradient
// at the pixel center instead of a solid color, then folds the analytic
// coverage into the premultiplied output. Single convex bounding fan + dense H/V
// band grids -> no band-seam double-count. Like the fill, each fragment maps its
// own pixel center into path space exactly (see `path_position_of_pixel`).

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
  // Band-grid parameters occupy two vec4-aligned rows.
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
  // Pixel-to-path mapping: `pathFromPixel` holds the inverse linear
  // transform's two columns; a pixel center `p` maps to
  // `pathFromPixel * (p - pixelOrigin) + pathOffset`.
  pathFromPixel: vec4f,
  pixelOrigin: vec2f,
  pathOffset: vec2f,
};

@group(0) @binding(0) var<uniform> uniforms: GradientUniforms;

struct Band {
  curveStart: u32,
  curveCount: u32,
};

@group(0) @binding(1) var<storage, read> bands: array<Band>;
@group(0) @binding(2) var<storage, read> curveData: array<f32>;
@group(0) @binding(3) var clipMaskTexture: texture_2d<f32>;
// Vertical bands, curves, and dense grids support the second analytic ray.
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
};

// Path-space position of a pixel center. Subtracting the draw's integer pixel
// origin first is exact, so the result depends only on the pixel's offset from
// the draw, not on where the draw lands in its target.
fn path_position_of_pixel(pixel_center: vec2f) -> vec2f {
  let local = pixel_center - uniforms.pixelOrigin;
  return vec2f(uniforms.pathFromPixel.x * local.x + uniforms.pathFromPixel.z * local.y,
               uniforms.pathFromPixel.y * local.x + uniforms.pathFromPixel.w * local.y) +
         uniforms.pathOffset;
}

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
  return out;
}

// ============================================================================
// Quadratic root solving + analytic per-ray coverage (mirrors SlugFillSource.h)
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

// Solve a monotone quadratic axis at an owned sample. Endpoint identity must survive
// coefficient rounding, and only an exactly linear polynomial may use a linear solve.
fn solve_quadratic(start: f32, control: f32, end: f32, sample: f32) -> vec2f {
  var roots = vec2f(-1.0, -1.0);
  if (!all(abs(vec4f(start, control, end, sample)) <= vec4f(3.402823466e38))) {
    return roots;
  }
  if (sample == start) {
    return vec2f(0.0, -1.0);
  }
  if (sample == end) {
    return vec2f(1.0, -1.0);
  }

  var a = (start - control) + (end - control);
  var b = 2.0 * (control - start);
  var c = start - sample;
  if (!all(abs(vec3f(a, b, c)) <= vec3f(3.402823466e38))) {
    return roots;
  }
  let coefficient_scale = max(max(abs(a), abs(b)), abs(c));
  // Rescale extreme coefficients before squaring to avoid overflow and underflow.
  if (coefficient_scale > 1e15 || (coefficient_scale > 0.0 && coefficient_scale < 1e-15)) {
    a = a / coefficient_scale;
    b = b / coefficient_scale;
    c = c / coefficient_scale;
  }
  if (a == 0.0) {
    if (b != 0.0) {
      let t = -c / b;
      if (t >= 0.0 && t <= 1.0) {
        roots.x = t;
      }
    }
    return roots;
  }

  let disc = b * b - 4.0 * a * c;
  if (!(disc >= 0.0)) {
    return roots;
  }
  let sqrt_disc = sqrt(disc);
  let q = -0.5 * (b + select(-sqrt_disc, sqrt_disc, b >= 0.0));
  if (q == 0.0) {
    let t = -b * (0.5 / a);
    if (t >= 0.0 && t <= 1.0) {
      roots.x = t;
    }
    return roots;
  }
  let t0 = q / a;
  let t1 = c / q;
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
  legacyCov: f32,
  legacyWgt: f32,
  complete: bool,
};

// Fixed capacity bounds per-fragment sorting without truncating the fallback winding scan.
const kMaxRayEvents: u32 = 32u;

struct RayEvents {
  values: array<vec2f, kMaxRayEvents>,
  count: u32,
  farWinding: i32,
  complete: bool,
};

fn empty_ray() -> RayCoverage {
  return RayCoverage(0.0, 0.0, 0.0, 0.0, 0.0, true);
}

fn add_ray_event(events: ptr<function, RayEvents>, r: f32, direction: f32) {
  if (!(*events).complete) {
    return;
  }
  if (!(abs(r) <= 3.402823466e38)) {
    (*events).complete = false;
    return;
  }
  if (r >= 0.5) {
    (*events).farWinding += i32(direction);
    return;
  }
  if (r <= -0.5) {
    return;
  }
  if ((*events).count == kMaxRayEvents) {
    (*events).complete = false;
    return;
  }

  var j = (*events).count;
  loop {
    if (j == 0u) {
      break;
    }
    if ((*events).values[j - 1u].x >= r) {
      break;
    }
    (*events).values[j] = (*events).values[j - 1u];
    j -= 1u;
  }
  (*events).values[j] = vec2f(r, direction);
  (*events).count += 1u;
}

// Integrate the NonZero predicate; internal winding changes are not boundary edges.
fn finish_ray(legacy: RayCoverage, events: ptr<function, RayEvents>) -> RayCoverage {
  var result = legacy;
  result.legacyCov = legacy.cov;
  result.legacyWgt = legacy.wgt;
  result.complete = (*events).complete;
  if (!result.complete) {
    return result;
  }

  result.cov = 0.0;
  result.wgt = 0.0;
  var cursor = 0.5;
  var winding = (*events).farWinding;
  var i = 0u;
  while (i < (*events).count) {
    let r = (*events).values[i].x;
    let wasInside = winding != 0;
    result.cov += (cursor - r) * select(0.0, 1.0, wasInside);
    var delta = 0;
    loop {
      delta += i32((*events).values[i].y);
      i += 1u;
      if (i == (*events).count) {
        break;
      }
      if ((*events).values[i].x != r) {
        break;
      }
    }
    winding += delta;
    if (wasInside != (winding != 0)) {
      result.wgt = max(result.wgt, saturate(1.0 - abs(r) * 2.0));
    }
    cursor = r;
  }
  result.cov += (cursor + 0.5) * select(0.0, 1.0, winding != 0);
  return result;
}

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

fn accumulateHoriz(slot: u32, sample: vec2f, ppemX: f32, collectNonzero: bool) -> RayCoverage {
  var result = empty_ray();
  var events: RayEvents;
  events.complete = collectNonzero && all(abs(sample) <= vec2f(3.402823466e38)) &&
                    ppemX > 0.0 && ppemX <= 3.402823466e38;
  if (slot == kNoBand) {
    return result;
  }
  let band = bands[slot];
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_h_curve(hCurveIndices[band.curveStart + i]);
    if (!all(abs(vec4f(curve.p0, curve.p1)) <= vec4f(3.402823466e38)) ||
        !all(abs(curve.p2) <= vec2f(3.402823466e38))) {
      events.complete = false;
    }
    let curve_max_x = max(curve.p0.x, max(curve.p1.x, curve.p2.x));
    if ((curve_max_x - sample.x) * ppemX <= -0.5) {
      break;
    }
    if (!owns_axis_sample(curve.p0.y, curve.p2.y, sample.y)) {
      continue;
    }
    let roots = solve_quadratic(curve.p0.y, curve.p1.y, curve.p2.y, sample.y);
    for (var k = 0; k < 2; k = k + 1) {
      let t = select(roots.y, roots.x, k == 0);
      if (t < 0.0) {
        continue;
      }
      let omt = 1.0 - t;
      let x = omt * omt * curve.p0.x + 2.0 * omt * t * curve.p1.x + t * t * curve.p2.x;
      let r = (x - sample.x) * ppemX;
      // Endpoint order preserves winding when the tangent derivative is zero.
      let s = select(-1.0, 1.0, curve.p2.y > curve.p0.y);
      // Preserve coincident constant-coordinate edges before grouping local crossings.
      let eventCoordinate = select(x, curve.p0.x,
          curve.p0.x == curve.p1.x && curve.p1.x == curve.p2.x);
      add_ray_event(&events, (eventCoordinate - sample.x) * ppemX, s);
      result.cov = result.cov + s * saturate(r + 0.5);
      result.wgt = max(result.wgt, saturate(1.0 - abs(r) * 2.0));
      result.winding = result.winding + s * select(0.0, 1.0, r >= 0.0);
    }
  }
  return finish_ray(result, &events);
}

fn accumulateVert(slot: u32, sample: vec2f, ppemY: f32, collectNonzero: bool) -> RayCoverage {
  var result = empty_ray();
  var events: RayEvents;
  events.complete = collectNonzero && all(abs(sample) <= vec2f(3.402823466e38)) &&
                    ppemY > 0.0 && ppemY <= 3.402823466e38;
  if (slot == kNoBand) {
    return result;
  }
  let band = vBands[slot];
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_v_curve(vCurveIndices[band.curveStart + i]);
    if (!all(abs(vec4f(curve.p0, curve.p1)) <= vec4f(3.402823466e38)) ||
        !all(abs(curve.p2) <= vec2f(3.402823466e38))) {
      events.complete = false;
    }
    let curve_max_y = max(curve.p0.y, max(curve.p1.y, curve.p2.y));
    if ((curve_max_y - sample.y) * ppemY <= -0.5) {
      break;
    }
    if (!owns_axis_sample(curve.p0.x, curve.p2.x, sample.x)) {
      continue;
    }
    let roots = solve_quadratic(curve.p0.x, curve.p1.x, curve.p2.x, sample.x);
    for (var k = 0; k < 2; k = k + 1) {
      let t = select(roots.y, roots.x, k == 0);
      if (t < 0.0) {
        continue;
      }
      let omt = 1.0 - t;
      let y = omt * omt * curve.p0.y + 2.0 * omt * t * curve.p1.y + t * t * curve.p2.y;
      let r = (y - sample.y) * ppemY;
      // Endpoint order preserves winding when the tangent derivative is zero.
      let s = select(1.0, -1.0, curve.p2.x > curve.p0.x);
      // Preserve coincident constant-coordinate edges before grouping local crossings.
      let eventCoordinate = select(y, curve.p0.y,
          curve.p0.y == curve.p1.y && curve.p1.y == curve.p2.y);
      add_ray_event(&events, (eventCoordinate - sample.y) * ppemY, s);
      result.cov = result.cov + s * saturate(r + 0.5);
      result.wgt = max(result.wgt, saturate(1.0 - abs(r) * 2.0));
      result.winding = result.winding + s * select(0.0, 1.0, r >= 0.0);
    }
  }
  return finish_ray(result, &events);
}

fn calc_coverage(h: RayCoverage, v: RayCoverage) -> f32 {
  let blended = abs(h.cov * h.wgt + v.cov * v.wgt) / max(h.wgt + v.wgt, 1.0 / 65536.0);
  let floor_cov = min(abs(h.cov), abs(v.cov));
  return max(blended, floor_cov);
}

// Dense local intersections retain the complete legacy result for both rays.
fn fill_coverage(horizontal: RayCoverage, vertical: RayCoverage,
                 fillRule: u32, antialias: u32) -> f32 {
  if (antialias == 0u) {
    let winding = u32(abs(horizontal.winding));
    if (fillRule == 0u) {
      return select(0.0, 1.0, winding != 0u);
    }
    return f32(winding & 1u);
  }
  var h = horizontal;
  var v = vertical;
  if (fillRule != 0u || !h.complete || !v.complete) {
    h.cov = h.legacyCov;
    h.wgt = h.legacyWgt;
    v.cov = v.legacyCov;
    v.wgt = v.legacyWgt;
  }
  let coverage = calc_coverage(h, v);
  if (fillRule == 0u) {
    return saturate(coverage);
  }
  return 1.0 - abs(1.0 - fract(coverage * 0.5) * 2.0);
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
    let linear_parameter = Ce / (2.0 * B);
    if (Fr + linear_parameter * Dr < 0.0) {
      return kInvalidGradientT;
    }
    return linear_parameter;
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
  // An all-zero mapping marks a transform without an inverse: the draw covers no
  // area, though its float-built enclosure may still rasterize a sliver.
  if (all(abs(uniforms.pathFromPixel) <= vec4f(0.0))) {
    discard;
  }
  let sample = path_position_of_pixel(pixel_center);
  // Pixels per path unit, per axis: the reciprocal of the mapping's `fwidth`.
  let ppem = 1.0 / (abs(uniforms.pathFromPixel.xy) + abs(uniforms.pathFromPixel.zw));

  var hCov = empty_ray();
  if (uniforms.hBandCount > 0u) {
    let hi = clamp(i32((sample.y - uniforms.yBase) / uniforms.hStride),
                   0, i32(uniforms.hBandCount) - 1);
    hCov = accumulateHoriz(hBandGrid[hi], sample, ppem.x,
                           uniforms.fillRule == 0u && uniforms.antialias != 0u);
  }

  var vCov = empty_ray();
  if (uniforms.vBandCount > 0u) {
    let vj = clamp(i32((sample.x - uniforms.xBase) / uniforms.vStride),
                   0, i32(uniforms.vBandCount) - 1);
    vCov = accumulateVert(vBandGrid[vj], sample, ppem.y,
                           uniforms.fillRule == 0u && uniforms.antialias != 0u);
  }

  var coverage = fill_coverage(hCov, vCov, uniforms.fillRule, uniforms.antialias);

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

  let raw_t = gradient_t(sample);
  if (raw_t < -1e20) {
    discard;
  }
  let t = apply_spread(raw_t, uniforms.spreadMode);
  let straight = sample_stops(t);

  var out: FragOutput;
  out.color = vec4f(straight.rgb * straight.a, straight.a) * coverage;
  return out;
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
