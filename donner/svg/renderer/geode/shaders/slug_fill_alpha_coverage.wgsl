// Alpha-coverage variant of the Slug fill shader.
//
// Identical to slug_fill.wgsl except:
//   * FragOutput has no @builtin(sample_mask) — avoids a hang on Intel Arc
//     (Mesa ANV / Xe KMD) when multiple band quads overlap at a pixel.
//   * Coverage is instead folded into the output color as
//     `color *= popcount(mask) / 4.0`, producing equivalent AA via alpha
//     blending on a single-sample render target.
//
// This shader is selected at pipeline creation time when
// `GeodeDevice::useAlphaCoverageAA()` returns true (Intel + Vulkan).
// See slug_fill.wgsl for the full algorithm commentary.

// ============================================================================
// Uniforms
// ============================================================================

struct Uniforms {
  mvp: mat4x4f,
  patternFromPath: mat4x4f,
  viewport: vec2f,
  tileSize: vec2f,
  color: vec4f,
  fillRule: u32,
  paintMode: u32,
  patternOpacity: f32,
  hasClipPolygon: u32,
  hasClipMask: u32,
  _pad0: u32,
  _pad1: u32,
  clipPolygonPlanes: array<vec4f, 4>,
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
@group(0) @binding(2) var<storage, read> curveData: array<f32>;
@group(0) @binding(3) var patternTexture: texture_2d<f32>;
@group(0) @binding(4) var patternSampler: sampler;
@group(0) @binding(5) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(6) var clipMaskSampler: sampler;

// Per-instance affine transform (Milestone 6 Bullet 2). See slug_fill.wgsl
// for the full comment — this binding/struct mirror that shader.
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
// Vertex stage (identical to slug_fill.wgsl)
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
fn vs_main(@builtin(instance_index) instance_index: u32, in: VertexInput) -> VertexOutput {
  let xf = instanceTransforms[instance_index];
  let instance_mat = mat4x4f(
    vec4f(xf.row0.x, xf.row1.x, 0.0, 0.0),
    vec4f(xf.row0.y, xf.row1.y, 0.0, 0.0),
    vec4f(0.0,       0.0,       1.0, 0.0),
    vec4f(xf.row0.z, xf.row1.z, 0.0, 1.0),
  );
  let effective_mvp = uniforms.mvp * instance_mat;

  let world_normal = (effective_mvp * vec4f(in.normal, 0.0, 0.0)).xy;
  let viewport_normal = world_normal * uniforms.viewport * 0.5;
  let viewport_len = length(viewport_normal);
  let d = 1.0 / max(viewport_len, 0.001);

  let dilated = in.pos + in.normal * d;

  var out: VertexOutput;
  out.clip_pos = effective_mvp * vec4f(dilated, 0.0, 1.0);
  out.sample_pos = dilated;
  out.bandIndex = in.bandIndex;
  return out;
}

// ============================================================================
// Quadratic Bézier ray intersection (identical to slug_fill.wgsl)
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

  var out: FragOutput;

  if (uniforms.paintMode == 0u) {
    // Scale premultiplied color by coverage (all 4 channels since premultiplied).
    out.color = uniforms.color * coverage * clipCoverage;
    return out;
  }

  // Pattern mode: sample the tile and scale by coverage.
  let patternPos = (uniforms.patternFromPath * vec4f(in.sample_pos, 0.0, 1.0)).xy;
  let wrapped = vec2f(
    fract(patternPos.x / uniforms.tileSize.x) * uniforms.tileSize.x,
    fract(patternPos.y / uniforms.tileSize.y) * uniforms.tileSize.y,
  );
  let uv = wrapped / uniforms.tileSize;
  var sampled = textureSample(patternTexture, patternSampler, uv);
  sampled = sampled * uniforms.patternOpacity * clipCoverage * coverage;
  out.color = sampled;
  return out;
}
