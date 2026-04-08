#include "donner/svg/renderer/geode/GeodeShaders.h"

namespace donner::geode {

// The WGSL source is embedded as a raw string literal to avoid needing a
// runtime file-loading path (and to keep the binary self-contained).
// Keep this in sync with donner/svg/renderer/geode/shaders/slug_fill.wgsl.
const std::string_view kSlugFillShaderWgsl = R"WGSL(
// Slug fill pipeline — see shaders/slug_fill.wgsl for documented source.

struct Uniforms {
  mvp: mat4x4f,
  viewport: vec2f,
  color: vec4f,
  fillRule: u32,
  _pad: vec3u,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

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

  if (abs(a) < 1e-6) {
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

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
  let band = bands[in.bandIndex];

  var winding: i32 = 0;
  for (var i = 0u; i < band.curveCount; i = i + 1u) {
    let curve = load_curve(band.curveStart + i);
    winding = winding + curve_winding(curve, in.sample_pos);
  }

  var inside: bool;
  if (uniforms.fillRule == 0u) {
    inside = winding != 0;
  } else {
    inside = (winding & 1) != 0;
  }

  if (!inside) {
    discard;
  }

  return uniforms.color;
}
)WGSL";

wgpu::ShaderModule createSlugFillShader(const wgpu::Device& device) {
  wgpu::ShaderSourceWGSL wgslSource = {};
  wgslSource.code = kSlugFillShaderWgsl.data();

  wgpu::ShaderModuleDescriptor desc = {};
  desc.label = "SlugFill";
  desc.nextInChain = &wgslSource;

  return device.CreateShaderModule(&desc);
}

}  // namespace donner::geode
