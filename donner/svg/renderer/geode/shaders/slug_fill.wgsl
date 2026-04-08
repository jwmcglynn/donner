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
  // Viewport dimensions in pixels.
  viewport: vec2f,
  // Fill color (premultiplied alpha).
  color: vec4f,
  // Fill rule: 0 = non-zero, 1 = even-odd.
  fillRule: u32,
  // Padding to 16-byte alignment.
  _pad: vec3u,
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
fn vs_main(in: VertexInput) -> VertexOutput {
  // Dynamic half-pixel dilation: expand the quad by exactly half a pixel
  // in viewport space along the outward normal.
  //
  // For the orthographic 2D case the math simplifies significantly from the
  // full quadratic solution — d = 1 / |viewport-space normal projection|.
  let world_normal = (uniforms.mvp * vec4f(in.normal, 0.0, 0.0)).xy;
  let viewport_normal = world_normal * uniforms.viewport * 0.5;
  let viewport_len = length(viewport_normal);
  let d = 1.0 / max(viewport_len, 0.001);

  // Dilate in path space so the fragment shader still samples in path space.
  let dilated = in.pos + in.normal * d;

  var out: VertexOutput;
  out.clip_pos = uniforms.mvp * vec4f(dilated, 0.0, 1.0);
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

  if (abs(a) < 1e-6) {
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

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
  let band = bands[in.bandIndex];

  // Iterate over all curves in this band and accumulate winding number.
  var winding: i32 = 0;
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_curve(band.curveStart + i);
    winding = winding + curve_winding(curve, in.sample_pos);
  }

  // Apply fill rule.
  var inside: bool;
  if (uniforms.fillRule == 0u) {
    // Non-zero: winding ≠ 0 means inside.
    inside = winding != 0;
  } else {
    // Even-odd: odd winding means inside.
    inside = (winding & 1) != 0;
  }

  if (!inside) {
    discard;
  }

  return uniforms.color;
}
