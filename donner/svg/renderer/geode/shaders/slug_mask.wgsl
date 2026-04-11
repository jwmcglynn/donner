// Slug mask pipeline: renders filled paths as coverage into an R8Unorm
// mask texture, for use as a clip source by the main fill / gradient
// pipelines (Phase 3b path clipping).
//
// This is a stripped-down copy of `slug_fill.wgsl`:
//   * Same vertex shader + same band/curve encoding, so the CPU-side
//     `GeodePathEncoder::encode` output can be fed directly in.
//   * Fragment stage does the same 4-sample `sample_mask` coverage test
//     as `slug_fill.wgsl`, but writes `vec4f(1.0, 0, 0, 0)` to an
//     R8Unorm color attachment instead of a full RGBA fill colour.
//   * 4× MSAA resolve averages the surviving samples into a single
//     fractional-alpha coverage per mask pixel, so downstream shaders
//     can do a straight `textureSample` to read clip coverage.
//   * No paint mode, no pattern, no clip polygon — the mask is a pure
//     CPU-uploaded path filled into empty space. If multiple clip paths
//     share a single layer they are unioned via `BlendOperation::Max` on
//     the host pipeline, not via shader logic.

// ============================================================================
// Uniforms
// ============================================================================

struct Uniforms {
  // Model-view-projection matrix. Same mvp as the fill pipeline — the
  // mask is rendered in path space and projected to mask-texture pixel
  // space exactly the way `slug_fill.wgsl` does.
  mvp: mat4x4f,
  // Viewport dimensions in pixels (mask texture size).
  viewport: vec2f,
  // Fill rule: 0 = non-zero, 1 = even-odd.
  fillRule: u32,
  // Nonzero when a nested clip mask is bound at binding 3 and should
  // be sampled during this draw. Used to support nested clip-path
  // references (`<clipPath>` whose children carry their own
  // `clip-path`). When the outer layer samples the inner layer's
  // already-rendered mask, each outer-layer shape ends up intersected
  // with the inner union — and Max blend unions the outer shapes on
  // top. When zero, a 1x1 dummy with value 1.0 is bound and the
  // shader effectively treats every pixel as fully unclipped.
  hasClipMask: u32,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

// ============================================================================
// Storage buffers
// ============================================================================

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

// Nested clip-mask input — see `uniforms.hasClipMask` above.
@group(0) @binding(3) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(4) var clipMaskSampler: sampler;

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
  // Dynamic half-pixel dilation — identical to slug_fill.wgsl.
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

  // Same `1e-4` degenerate-quadratic threshold as slug_fill.wgsl —
  // LineTo-derived quadratics have theoretical `a = 0` but float32
  // rounding leaves a tiny residual that, without the widened gate,
  // trips the quadratic branch and amplifies into spurious roots.
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

// ============================================================================
// Fragment stage
// ============================================================================

/// One color attachment + one sample mask. Same scheme as `slug_fill.wgsl`:
/// the fragment runs once per pixel, the per-sample coverage test populates
/// `mask` bits, and the hardware resolves the MSAA color attachment into
/// a fractional-alpha single-sample R8 texture — directly usable as a
/// clip coverage source.
struct FragOutput {
  @location(0) color: vec4f,
  @builtin(sample_mask) mask: u32,
};

@fragment
fn fs_main(in: VertexOutput) -> FragOutput {
  let band = bands[in.bandIndex];

  let pixel_center = in.clip_pos.xy;

  let dx = dpdx(in.sample_pos);
  let dy = dpdy(in.sample_pos);

  // Same rotated 4-sample pattern as `slug_fill.wgsl` so the mask's
  // edge AA lands on the same sub-pixel offsets as the colour pass.
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
    if (sample_is_inside(band, sp)) {
      mask = mask | (1u << s);
    }
  }

  if (mask == 0u) {
    discard;
  }

  // Nested clip-mask input: when a deeper layer's mask is bound, each
  // outer-layer shape we render must be INTERSECTED with the deeper
  // union. Sampling it as a multiplier on the coverage value turns the
  // Max-blended layer into `max(shape_i ∩ nested)` = `(union of
  // shape_i) ∩ nested`, which matches `RendererTinySkia`'s recursive
  // intersection rule.
  var clipCoverage: f32 = 1.0;
  if (uniforms.hasClipMask != 0u) {
    let mask_uv = pixel_center / uniforms.viewport;
    clipCoverage = clamp(textureSample(clipMaskTexture, clipMaskSampler, mask_uv).r,
                         0.0, 1.0);
    if (clipCoverage <= 0.0) {
      discard;
    }
  }

  var out: FragOutput;
  // Red-channel coverage. The alpha channel is ignored because the
  // downstream fill pipeline samples `.r` as the clip value. Multiply
  // by the nested clip coverage so shape edges land inside the deeper
  // union.
  out.color = vec4f(clipCoverage, 0.0, 0.0, 1.0);
  out.mask = mask;
  return out;
}
