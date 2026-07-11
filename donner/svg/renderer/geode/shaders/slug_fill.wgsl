// Slug fill pipeline: analytic dual-ray coverage at 1 sample/pixel.
//
// Algorithm:
//   1. The path is decomposed on the CPU into horizontal bands (Y-monotonic
//      curves, for the horizontal ray) AND vertical bands (X-monotonic curves,
//      for the vertical ray). Dense band grids map a sample position to its
//      band in O(1).
//   2. A SINGLE bounding quad (2 triangles) is drawn per path. The vertex
//      shader performs dynamic half-pixel dilation (the key Slug innovation)
//      so the quad expands by exactly half a pixel in viewport space.
//   3. Because each pixel is rasterized by exactly one fragment (one quad),
//      folded sampleCount=1 coverage composes correctly with no band-seam
//      double-count (Blocker B is structurally impossible).
//   4. The fragment shader casts a HORIZONTAL ray through the pixel's
//      horizontal band and a VERTICAL ray through its vertical band,
//      accumulating analytic coverage per Slug's `CalcCoverage`, then folds
//      the result into the premultiplied output color.
//
// Curves are axis-monotonic quadratic Béziers (3 control points each); each
// curve crosses a given ray at most once. The per-root coverage is
// `saturate(r + 0.5)` (signed by winding direction) with weight
// `saturate(1 - 2|r|)`, where `r` is the signed root distance from the pixel
// center in pixels (`pixelsPerEm = 1/fwidth(sample_pos)`).

// ============================================================================
// Uniforms
// ============================================================================

struct Uniforms {
  // Model-view-projection matrix (2D content uses an orthographic affine).
  mvp: mat4x4f,
  // Pattern sampling transform (paintMode == 1 only).
  patternFromPath: mat4x4f,
  // Viewport dimensions in pixels.
  viewport: vec2f,
  // Pattern tile size in pattern-tile space.
  tileSize: vec2f,
  // Fill color (premultiplied alpha). Only used when paintMode == 0.
  color: vec4f,
  // Fill rule: 0 = non-zero, 1 = even-odd.
  fillRule: u32,
  // Paint mode: 0 = solid color, 1 = pattern texture (repeat-tiled).
  paintMode: u32,
  // Pattern alpha multiplier (e.g., fill-opacity). 1.0 for solid paint.
  patternOpacity: f32,
  // Nonzero when a convex 4-vertex clip polygon is active.
  hasClipPolygon: u32,
  // Nonzero when a path-clip mask texture is bound at binding 5.
  hasClipMask: u32,
  _pad0: u32,
  _pad1: u32,
  _pad2: u32,
  // Band-grid parameters (0041 §8.1). The horizontal grid bins the path's
  // Y-range into `hBandCount` strips of `hStride` starting at `yBase`; the
  // vertical grid bins the X-range into `vBandCount` strips of `vStride`
  // starting at `xBase`. Padded to 16 bytes (two vec4-aligned rows).
  yBase: f32,
  hStride: f32,
  hBandCount: u32,
  xBase: f32,
  vStride: f32,
  vBandCount: u32,
  _gridPad0: u32,
  _gridPad1: u32,
  // Four inward-facing half-planes in viewport-pixel space, one per polygon
  // edge. `plane.xyz = (nx, ny, c)` with `nx*x + ny*y + c >= 0` inside.
  clipPolygonPlanes: array<vec4f, 4>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

// ============================================================================
// Storage buffers
// ============================================================================

// Per-band metadata. With the dense-grid lookup the band's strip extents come
// from the grid params, so only (curveStart, curveCount) are meaningful; the
// remaining fields are retained for layout compatibility with the encoder's
// `EncodedPath::Band` (32 bytes).
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

// Horizontal bands + curves (for the horizontal ray).
@group(0) @binding(1) var<storage, read> bands: array<Band>;
@group(0) @binding(2) var<storage, read> curveData: array<f32>;

// Pattern tile texture + sampler.
@group(0) @binding(3) var patternTexture: texture_2d<f32>;
@group(0) @binding(4) var patternSampler: sampler;

// Path-clip mask texture + sampler.
@group(0) @binding(5) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(6) var clipMaskSampler: sampler;

// Per-instance affine transform (vertex stage only).
struct InstanceTransform {
  row0: vec4f,
  row1: vec4f,
};
@group(0) @binding(7) var<storage, read> instanceTransforms: array<InstanceTransform>;

// Vertical bands + curves (for the vertical ray) and the dense band grids
// (0041 §8.1). `hBandGrid[i]` maps grid cell i to a slot in `bands` (or the
// kNoBand sentinel); `vBandGrid[j]` maps cell j to a slot in `vBands`.
@group(0) @binding(8) var<storage, read> vBands: array<Band>;
@group(0) @binding(9) var<storage, read> vCurveData: array<f32>;
@group(0) @binding(10) var<storage, read> hBandGrid: array<u32>;
@group(0) @binding(11) var<storage, read> vBandGrid: array<u32>;

// Sentinel for an empty grid cell (must match EncodedPath::kNoBand).
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

struct VertexInput {
  @location(0) pos: vec2f,
  @location(1) normal: vec2f,
  @location(2) bandIndex: u32,  // Unused for the analytic path (single quad).
};

struct VertexOutput {
  @builtin(position) clip_pos: vec4f,
  // Path-space sample position. Fragment shader casts both rays from here.
  @location(0) sample_pos: vec2f,
};

@vertex
fn vs_main(@builtin(instance_index) instance_index: u32, in: VertexInput) -> VertexOutput {
  let xf = instanceTransforms[instance_index];
  let instance_mat = mat4x4f(
    vec4f(xf.row0.x, xf.row1.x, 0.0, 0.0),
    vec4f(xf.row0.y, xf.row1.y, 0.0, 0.0),
    vec4f(0.0,       0.0,       1.0, 0.0),
    vec4f(xf.row0.z, xf.row1.z, 0.0, 1.0),
  );
  let effective_mvp = uniforms.mvp * instance_mat;

  // Dynamic half-pixel dilation: expand the quad by exactly half a pixel in
  // viewport space along the outward normal.
  let world_normal = (effective_mvp * vec4f(in.normal, 0.0, 0.0)).xy;
  let viewport_normal = world_normal * uniforms.viewport * 0.5;
  let viewport_len = length(viewport_normal);
  let d = 1.0 / max(viewport_len, 0.001);

  let dilated = in.pos + in.normal * d;

  var out: VertexOutput;
  out.clip_pos = effective_mvp * vec4f(dilated, 0.0, 1.0);
  out.sample_pos = dilated;
  return out;
}

// ============================================================================
// Quadratic Bézier root solving
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

// Solve at² + bt + c = 0 for roots in [0, 1]. Returns two t values; invalid
// roots are set to -1. Numerically-stable Citardauq form (matches the Slug
// reference): dividing by the larger-magnitude root avoids catastrophic
// cancellation when |4ac| << b².
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

// ----------------------------------------------------------------------------
// Per-ray analytic coverage (0041 §1).
//
// The horizontal ray travels along +X at y = sample.y. For each Y-monotonic
// curve in the band we solve for the t where the curve's Y equals sample.y,
// evaluate the X distance from the pixel center, scale to pixels, and
// accumulate `saturate(r + 0.5)` signed by the crossing direction (sign of
// dy/dt - a downward crossing winds +1, upward −1, matching the integer
// `curve_winding` convention). The weight is `saturate(1 - 2|r|)`, reduced
// over the band via `max`.
//
// The vertical ray (`accumulateVert`) is the transpose: it travels along +Y
// at x = sample.x, uses X-monotonic curves, distances along X, and the sign of
// dx/dt.
// ----------------------------------------------------------------------------

struct RayCoverage {
  cov: f32,
  wgt: f32,
};

// A shared vertex belongs to the curve that starts there, not the curve that ends there.
// Testing the monotonic axis directly avoids backend-dependent root rounding near t=1.
fn owns_axis_sample(start: f32, end: f32, sample: f32) -> bool {
  return select(sample <= start && sample > end,
                sample >= start && sample < end,
                end > start);
}

fn accumulateHoriz(slot: u32, sample: vec2f, ppemX: f32) -> RayCoverage {
  var result: RayCoverage;
  result.cov = 0.0;
  result.wgt = 0.0;
  if (slot == kNoBand) {
    return result;
  }
  let band = bands[slot];
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_h_curve(band.curveStart + i);
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
      // Signed pixel distance of the crossing from the pixel center along +X.
      let r = (x - sample.x) * ppemX;
      let dy_dt = 2.0 * omt * (curve.p1.y - curve.p0.y) + 2.0 * t * (curve.p2.y - curve.p1.y);
      // Winding sign: +1 for a downward (increasing-Y) crossing.
      let s = select(-1.0, 1.0, dy_dt >= 0.0);
      result.cov = result.cov + s * saturate(r + 0.5);
      result.wgt = max(result.wgt, saturate(1.0 - abs(r) * 2.0));
    }
  }
  return result;
}

fn accumulateVert(slot: u32, sample: vec2f, ppemY: f32) -> RayCoverage {
  var result: RayCoverage;
  result.cov = 0.0;
  result.wgt = 0.0;
  if (slot == kNoBand) {
    return result;
  }
  let band = vBands[slot];
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_v_curve(band.curveStart + i);
    if (!owns_axis_sample(curve.p0.x, curve.p2.x, sample.x)) {
      continue;
    }
    // Solve for the t where the curve's X equals sample.x.
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
      // Signed pixel distance of the crossing from the pixel center along +Y.
      let r = (y - sample.y) * ppemY;
      let dx_dt = 2.0 * omt * (curve.p1.x - curve.p0.x) + 2.0 * t * (curve.p2.x - curve.p1.x);
      // Winding sign: +1 for a rightward (increasing-X) crossing. The vertical
      // ray's winding must agree with the horizontal ray's so the combine
      // blends like-signed coverage. A +X crossing at increasing X corresponds
      // to a +Y boundary on the perpendicular ray, so we negate to match the
      // horizontal convention (downward = positive).
      let s = select(1.0, -1.0, dx_dt >= 0.0);
      result.cov = result.cov + s * saturate(r + 0.5);
      result.wgt = max(result.wgt, saturate(1.0 - abs(r) * 2.0));
    }
  }
  return result;
}

// Combine the two rays' coverage (0041 §1, reference `CalcCoverage`). The
// weighted blend handles the general case; the `min(|xcov|,|ycov|)` floor
// resolves near-axis-aligned / thin-feature / crosshair cases that a single
// ray conflates.
fn calc_coverage(h: RayCoverage, v: RayCoverage) -> f32 {
  let blended = abs(h.cov * h.wgt + v.cov * v.wgt) / max(h.wgt + v.wgt, 1.0 / 65536.0);
  let floor_cov = min(abs(h.cov), abs(v.cov));
  return max(blended, floor_cov);
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

  // Path-units per pixel, per axis. `sample_pos` is a linear function of the
  // viewport position, so fwidth gives the constant per-pixel path delta.
  let ppem = 1.0 / fwidth(in.sample_pos);

  // Look up this pixel's horizontal and vertical bands in O(1).
  var hCov: RayCoverage;
  hCov.cov = 0.0;
  hCov.wgt = 0.0;
  if (uniforms.hBandCount > 0u) {
    let hi = clamp(i32((in.sample_pos.y - uniforms.yBase) / uniforms.hStride),
                   0, i32(uniforms.hBandCount) - 1);
    let slot = hBandGrid[hi];
    hCov = accumulateHoriz(slot, in.sample_pos, ppem.x);
  }

  var vCov: RayCoverage;
  vCov.cov = 0.0;
  vCov.wgt = 0.0;
  if (uniforms.vBandCount > 0u) {
    let vj = clamp(i32((in.sample_pos.x - uniforms.xBase) / uniforms.vStride),
                   0, i32(uniforms.vBandCount) - 1);
    let slot = vBandGrid[vj];
    vCov = accumulateVert(slot, in.sample_pos, ppem.y);
  }

  var coverage = calc_coverage(hCov, vCov);

  // Fill rule. Non-zero clamps the (signed) winding coverage; even-odd folds
  // the RAW coverage via a triangle wave (the fold must see the unsaturated
  // value - a hole has combined coverage ≈ 2, which the wave maps to 0).
  if (uniforms.fillRule == 0u) {
    coverage = saturate(coverage);
  } else {
    coverage = 1.0 - abs(1.0 - fract(coverage * 0.5) * 2.0);
  }

  // Convex clip-polygon test, in viewport-pixel space.
  if (!sample_in_clip_polygon(pixel_center)) {
    coverage = 0.0;
  }

  // Path-clip mask coverage (multiplicative).
  var clipCoverage: f32 = 1.0;
  if (uniforms.hasClipMask != 0u) {
    clipCoverage = clip_mask_coverage(pixel_center);
  }
  coverage = coverage * clipCoverage;

  if (coverage <= 0.0) {
    discard;
  }

  var out: FragOutput;

  if (uniforms.paintMode == 0u) {
    // `uniforms.color` is premultiplied; scale all channels by coverage.
    out.color = uniforms.color * coverage;
    return out;
  }

  // Pattern mode.
  let patternPos = (uniforms.patternFromPath * vec4f(in.sample_pos, 0.0, 1.0)).xy;
  let wrapped = vec2f(
    fract(patternPos.x / uniforms.tileSize.x) * uniforms.tileSize.x,
    fract(patternPos.y / uniforms.tileSize.y) * uniforms.tileSize.y,
  );
  let uv = wrapped / uniforms.tileSize;
  var sampled = textureSample(patternTexture, patternSampler, uv);
  sampled = sampled * uniforms.patternOpacity * coverage;
  out.color = sampled;
  return out;
}
