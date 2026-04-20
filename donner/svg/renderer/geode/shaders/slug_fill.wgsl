// Slug fill pipeline: renders filled paths on the GPU without tessellation
// or glyph atlases.
//
// Algorithm:
//   1. Each path is decomposed into horizontal bands on the CPU.
//   2. Each band has a bounding quad (2 triangles) drawn by the vertex shader.
//   3. The vertex shader performs dynamic half-pixel dilation (the key Slug
//      innovation) so the bounding quad expands by exactly half a pixel in
//      viewport space regardless of transform.
//   4. The fragment shader casts a horizontal ray from each pixel through the
//      quadratic Bézier curves in this band and accumulates winding numbers.
//   5. Fill rule (non-zero / even-odd) determines inside/outside; coverage is
//      computed from sub-pixel root distances for antialiasing.
//
// Curves are stored as Y-monotonic quadratic Béziers (3 control points each)
// in the curves storage buffer. A band references a contiguous slice via
// (curveStart, curveCount). This is the simplified post-2017 Slug band format
// — no bidirectional ray sorting, no band-splitting.

// ============================================================================
// Uniforms
// ============================================================================

struct Uniforms {
  // Model-view-projection matrix. 4x4 to support future 3D / perspective;
  // 2D content uses an orthographic matrix that's effectively 2x2 + translate.
  mvp: mat4x4f,
  // Pattern sampling transform: maps path-space sample positions to
  // pattern-tile-space. Only used when paintMode == 1 (pattern fill). Stored
  // as mat4x4 for alignment simplicity; only the upper-left 2x3 affine is
  // meaningful for 2D content.
  patternFromPath: mat4x4f,
  // Viewport dimensions in pixels.
  viewport: vec2f,
  // Pattern tile size in pattern-tile space (width, height). Ignored when
  // paintMode == 0.
  tileSize: vec2f,
  // Fill color (premultiplied alpha). Only used when paintMode == 0.
  color: vec4f,
  // Fill rule: 0 = non-zero, 1 = even-odd.
  fillRule: u32,
  // Paint mode: 0 = solid color, 1 = pattern texture (repeat-tiled).
  paintMode: u32,
  // Pattern alpha multiplier (e.g., fill-opacity). 1.0 for solid paint.
  patternOpacity: f32,
  // Nonzero when a convex 4-vertex clip polygon is active. When 0, the
  // `clipPolygonPlanes` field is ignored.
  hasClipPolygon: u32,
  // Nonzero when a path-clip mask texture is bound at binding 5 and
  // should be sampled for per-pixel clip coverage. When 0, the mask
  // binding still holds a 1x1 dummy texture (value 1.0) so the shader
  // can unconditionally sample without tripping WebGPU validation.
  hasClipMask: u32,
  _pad0: u32,
  _pad1: u32,
  // Four inward-facing half-planes in viewport-pixel space, one per polygon
  // edge. `plane.xyz = (nx, ny, c)` with `nx*x + ny*y + c >= 0` inside. The
  // `w` component is padding for std140-style alignment.
  clipPolygonPlanes: array<vec4f, 4>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

// ============================================================================
// Storage buffers
// ============================================================================

// Per-band metadata. Parallel to the draw instance — the vertex shader reads
// the band for its instance via gl_InstanceIndex / @builtin(instance_index).
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

// Quadratic Bézier control points: (p0, p1, p2). Stored as packed floats,
// 6 floats per curve.
//
// Using vec2f stride would waste memory due to alignment padding; the
// flat f32 array packs tighter at the cost of slightly more arithmetic.
@group(0) @binding(2) var<storage, read> curveData: array<f32>;

// Pattern tile texture + sampler. These bindings exist even when paintMode == 0
// so the bind group layout is stable across draw calls; in solid-paint mode a
// 1x1 dummy texture is bound and the shader never samples it.
@group(0) @binding(3) var patternTexture: texture_2d<f32>;
@group(0) @binding(4) var patternSampler: sampler;

// Path-clip mask texture (Phase 3b). R8Unorm, 1-sample (resolved from a
// 4× MSAA render target before this draw). Always bound — when
// `uniforms.hasClipMask == 0u` a 1x1 dummy texture with value 1.0 is
// bound so the shader can unconditionally `textureSample` at the pixel
// center without needing branchless paths or bind group layout
// variants. The sampler uses linear filtering so the clip edge
// interpolates smoothly across pixels.
@group(0) @binding(5) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(6) var clipMaskSampler: sampler;

// Per-instance affine transform (Milestone 6 Bullet 2: `<use>` instancing).
//
// Each `array` element is a 2×3 affine packed as two vec4f:
//   row0 = (a, c, e, _pad)  →  x' = a*x + c*y + e
//   row1 = (b, d, f, _pad)  →  y' = b*x + d*y + f
//
// The vertex shader composes this transform into `uniforms.mvp` before
// applying it to `pos`/`normal`, so a single instanced draw can paint N
// copies of the same encoded path at N different transforms.
//
// For non-instanced draws (instanceCount == 1) a device-owned 1-element
// buffer with identity transform is bound here so the bind-group layout
// stays stable across draw calls.
struct InstanceTransform {
  row0: vec4f,
  row1: vec4f,
};
@group(0) @binding(7) var<storage, read> instanceTransforms: array<InstanceTransform>;

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
  // Position in path space.
  @location(0) pos: vec2f,
  // Outward normal for dilation (quad corner sign, -1 or +1 per axis).
  @location(1) normal: vec2f,
  // Band index — which band this vertex belongs to.
  @location(2) bandIndex: u32,
};

struct VertexOutput {
  @builtin(position) clip_pos: vec4f,
  // Path-space sample position. Fragment shader casts a ray from here.
  @location(0) sample_pos: vec2f,
  // Band index passed through so the fragment shader can look up the band
  // (WGSL doesn't have per-primitive builtins on all backends).
  @location(1) @interpolate(flat) bandIndex: u32,
};

@vertex
fn vs_main(@builtin(instance_index) instance_index: u32, in: VertexInput) -> VertexOutput {
  // Compose the per-instance affine transform into the MVP. For
  // non-instanced draws `instance_index == 0` and the identity
  // transform bound by the encoder makes this a no-op. For a
  // `fillPathInstanced` draw each invocation picks up its own
  // transform, so N copies of the same encoded path land at N
  // different screen positions with a single draw call.
  let xf = instanceTransforms[instance_index];
  // WGSL matrices are column-major: mat4x4f(col0, col1, col2, col3).
  //   col 0: (a, b, 0, 0)   col 1: (c, d, 0, 0)
  //   col 2: (0, 0, 1, 0)   col 3: (e, f, 0, 1)
  let instance_mat = mat4x4f(
    vec4f(xf.row0.x, xf.row1.x, 0.0, 0.0),
    vec4f(xf.row0.y, xf.row1.y, 0.0, 0.0),
    vec4f(0.0,       0.0,       1.0, 0.0),
    vec4f(xf.row0.z, xf.row1.z, 0.0, 1.0),
  );
  let effective_mvp = uniforms.mvp * instance_mat;

  // Dynamic half-pixel dilation: expand the quad by exactly half a pixel
  // in viewport space along the outward normal.
  //
  // For the orthographic 2D case the math simplifies significantly from the
  // full quadratic solution — d = 1 / |viewport-space normal projection|.
  let world_normal = (effective_mvp * vec4f(in.normal, 0.0, 0.0)).xy;
  let viewport_normal = world_normal * uniforms.viewport * 0.5;
  let viewport_len = length(viewport_normal);
  let d = 1.0 / max(viewport_len, 0.001);

  // Dilate in path space so the fragment shader still samples in path space.
  // (Sample space is the path's own coordinate system — the fragment ray-cast
  // against `curveData` must stay pre-transform; `effective_mvp` only drives
  // clip-space positioning.)
  let dilated = in.pos + in.normal * d;

  var out: VertexOutput;
  out.clip_pos = effective_mvp * vec4f(dilated, 0.0, 1.0);
  out.sample_pos = dilated;
  out.bandIndex = in.bandIndex;
  return out;
}

// ============================================================================
// Quadratic Bézier ray intersection
// ============================================================================

// Unpack a quadratic from the curve buffer. Each curve is 6 floats:
// [p0x, p0y, p1x, p1y, p2x, p2y].
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

// Solve the quadratic equation at² + bt + c = 0 for roots in (0, 1].
// Returns two t values; invalid roots are set to -1.
fn solve_quadratic(a: f32, b: f32, c: f32) -> vec2f {
  var roots = vec2f(-1.0, -1.0);

  // Threshold raised from 1e-6 to 1e-4 to robustly route degenerate
  // (LineTo-derived) quadratics through the linear branch. Those curves
  // have theoretical a = p0y - 2*p1y + p2y = 0 (since p1 = midpoint),
  // but float32 round-off in the encoder produces |a| up to ~1.5e-5 in
  // practice. With a 1e-6 discriminator threshold, the quadratic branch
  // fires with a tiny `a`, amplifying noise into spurious root positions
  // that manifest as scattered single-pixel artifacts inside stroked
  // closed shapes (the Phase 2E "residual scatter" bug). Genuine
  // quadratics from cubic-to-quadratic decomposition or text glyphs have
  // |a| >> 1e-4 in all realistic geometry, so this threshold is safe.
  if (abs(a) < 1e-4) {
    // Linear fallback: bt + c = 0 → t = -c/b.
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

// Intersect a horizontal ray (y = sample_y, x >= sample_x) with a quadratic
// Bézier and return the winding number contribution (+1 or -1 per valid
// root, 0 if none cross).
//
// A quadratic Bézier in Y is: y(t) = (1-t)² p0y + 2(1-t)t p1y + t² p2y
// Rearranging: t² (p0y - 2 p1y + p2y) + 2t (p1y - p0y) + (p0y - sample_y) = 0
// i.e., a t² + b t + c = 0 with a = p0y - 2 p1y + p2y, b = 2(p1y - p0y),
// c = p0y - sample_y.
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

    // Evaluate x(t) and tangent dy/dt to determine crossing direction.
    let omt = 1.0 - t;
    let x = omt * omt * curve.p0.x + 2.0 * omt * t * curve.p1.x + t * t * curve.p2.x;
    if (x < sample.x) {
      continue;
    }

    // Crossing direction: sign of dy/dt at t.
    // dy/dt = 2 (1-t) (p1y - p0y) + 2 t (p2y - p1y)
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
// Fragment stage
// ============================================================================

/// Test whether a sample position (in viewport-pixel space) lies inside
/// the active convex clip polygon. Returns `true` when no polygon is
/// active, so callers can unconditionally AND this into their coverage
/// decision.
fn sample_in_clip_polygon(pixel_pos: vec2f) -> bool {
  if (uniforms.hasClipPolygon == 0u) {
    return true;
  }
  for (var i = 0u; i < 4u; i = i + 1u) {
    let plane = uniforms.clipPolygonPlanes[i];
    // Small epsilon so samples exactly on the polygon boundary count as
    // inside — matches the inclusive-at-boundary convention of the
    // scissor rect fallback.
    if (plane.x * pixel_pos.x + plane.y * pixel_pos.y + plane.z < -1e-4) {
      return false;
    }
  }
  return true;
}

/// Test whether a single sub-pixel sample position is inside the fill,
/// per the active fill rule. Shared between `fs_main` sample-mask loop
/// and any future per-sample helpers.
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

/// Fragment output with a sample-mask so the pipeline's 4× MSAA can
/// produce sub-pixel fractional coverage. The fragment shader runs once
/// per pixel, computes per-sample inside/outside via `sample_is_inside`
/// at four offsets around the pixel center, and writes the same color
/// to the samples that are "inside". The hardware resolve step averages
/// the samples into the 1-sample resolve attachment — the result is
/// 0/4, 1/4, 2/4, 3/4, or 4/4 coverage per pixel, closely matching
/// tiny-skia's 4× supersampling scan-converter.
struct FragOutput {
  @location(0) color: vec4f,
  @builtin(sample_mask) mask: u32,
};

@fragment
fn fs_main(in: VertexOutput) -> FragOutput {
  let band = bands[in.bandIndex];

  // Framebuffer-pixel center of this fragment. WGSL specifies that
  // reading `@builtin(position)` in the fragment stage returns the pixel
  // coordinates (x + 0.5, y + 0.5) — this is what the clip polygon's
  // half-plane equations are expressed in.
  let pixel_center = in.clip_pos.xy;

  // NO pixel-center band-Y clip. The previous single-sample path
  // discarded fragments whose pixel center fell outside the band's
  // half-open `[yMin, yMax)` range to avoid double-counting curves at
  // the band overlap boundary. That approach doesn't generalise to
  // sub-pixel coverage: a pixel whose CENTER sits above the boundary
  // may still have sub-pixel samples *below* it that should be filled
  // by the adjacent band, and vice versa. Instead, the per-sample
  // band-Y check inside the loop below owns each sample to exactly
  // one band — if sample N's Y is outside `band.y` range, the bit is
  // left clear and the neighbouring band's fragment invocation at the
  // same viewport pixel sets it. The sum across both bands' fragment
  // outputs (blended via premultiplied source-over with mask writes)
  // reproduces a conflict-free 4-sample coverage at boundary pixels.

  // Derivatives of path-space `sample_pos` with respect to viewport
  // pixels. `sample_pos` is a linear function of the viewport position
  // (the vertex shader applies the MVP, the rasterizer interpolates),
  // so `dpdx` / `dpdy` are constant across the primitive and give the
  // path-space delta per one viewport pixel along each axis.
  let dx = dpdx(in.sample_pos);
  let dy = dpdy(in.sample_pos);

  // Four sub-pixel offsets, in viewport-pixel units from the pixel
  // center. WebGPU doesn't promise any specific MSAA sample pattern,
  // but the sample_mask bits still map bit N → sample index N. The
  // resolve step averages the 4 samples regardless of their geometric
  // positions, so any 4 distinct in-pixel offsets give a correct 4×
  // coverage estimate. Use D3D-style rotated positions to avoid
  // axis-aligned Moiré on near-horizontal / near-vertical edges.
  var offsets = array<vec2f, 4>(
    vec2f(-0.125, -0.375),
    vec2f( 0.375, -0.125),
    vec2f(-0.375,  0.125),
    vec2f( 0.125,  0.375),
  );

  var mask: u32 = 0u;
  for (var s: u32 = 0u; s < 4u; s = s + 1u) {
    // Convert the viewport-pixel offset into a path-space delta.
    let sp = in.sample_pos + offsets[s].x * dx + offsets[s].y * dy;

    // Per-sample band-Y clip: samples on the other side of the band
    // boundary are OWNED by the adjacent band's fragment invocation at
    // this same pixel. Skipping them here (leaving their sample_mask
    // bit at 0) means the other band will fill them in. No double
    // coverage at band boundaries.
    if (sp.y < band.yMin || sp.y >= band.yMax) {
      continue;
    }

    // Convex clip-polygon test, in viewport-pixel space. The polygon
    // planes were uploaded by `GeoEncoder::setClipPolygon`; no-op when
    // inactive.
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

  // Path-clip mask: sample the pre-rendered clip mask texture at the
  // pixel center and fold its coverage into the fragment colour. The
  // mask texture was resolved from a 4× MSAA render target so its
  // alpha channel already carries fractional edge coverage; sampling
  // with a linear filter interpolates that across pixel boundaries.
  // When no mask is active, `hasClipMask == 0` and we skip the work
  // entirely — the dummy texture's 1.0 value would also work but the
  // branch shaves a texture fetch off the hot path.
  var clipCoverage: f32 = 1.0;
  if (uniforms.hasClipMask != 0u) {
    clipCoverage = clip_mask_coverage(pixel_center);
    if (clipCoverage <= 0.0) {
      discard;
    }
  }

  var out: FragOutput;
  out.mask = mask;

  if (uniforms.paintMode == 0u) {
    // `uniforms.color` is already premultiplied by the host encoder.
    // The hardware routes this one color to the selected samples and
    // resolves to fractional coverage after the pass.
    out.color = uniforms.color * clipCoverage;
    return out;
  }

  // Pattern mode: sample the tile at the pixel center. The
  // sample_mask still controls edge coverage.
  let patternPos = (uniforms.patternFromPath * vec4f(in.sample_pos, 0.0, 1.0)).xy;
  let wrapped = vec2f(
    fract(patternPos.x / uniforms.tileSize.x) * uniforms.tileSize.x,
    fract(patternPos.y / uniforms.tileSize.y) * uniforms.tileSize.y,
  );
  let uv = wrapped / uniforms.tileSize;
  var sampled = textureSample(patternTexture, patternSampler, uv);
  sampled = sampled * uniforms.patternOpacity * clipCoverage;
  out.color = sampled;
  return out;
}
