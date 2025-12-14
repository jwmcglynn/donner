struct GeodeSegment {
  p0 : vec2f;
  p1 : vec2f;
  p2 : vec2f;
  kind : u32; // 0 = line, 1 = quadratic
  pad : u32;
};

struct FrameUniforms {
  boundsMin : vec2f;
  boundsMax : vec2f;
  viewportSize : vec2f;
  segmentCount : u32;
  _pad : vec3u;
};

struct VertexOutput {
  @builtin(position) position : vec4f;
  @location(0) localPosition : vec2f;
};

@group(0) @binding(0) var<storage, read> segments : array<GeodeSegment>;
@group(0) @binding(1) var<uniform> frame : FrameUniforms;

fn toClipSpace(pos : vec2f) -> vec4f {
  let ndc = vec2f(
    (pos.x / frame.viewportSize.x) * 2.0 - 1.0,
    1.0 - (pos.y / frame.viewportSize.y) * 2.0,
  );
  return vec4f(ndc, 0.0, 1.0);
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {
  var quad = array<vec2f, 6>(
    vec2f(frame.boundsMin.x, frame.boundsMin.y),
    vec2f(frame.boundsMax.x, frame.boundsMin.y),
    vec2f(frame.boundsMin.x, frame.boundsMax.y),
    vec2f(frame.boundsMax.x, frame.boundsMin.y),
    vec2f(frame.boundsMax.x, frame.boundsMax.y),
    vec2f(frame.boundsMin.x, frame.boundsMax.y),
  );

  var out : VertexOutput;
  out.localPosition = quad[vertexIndex];
  out.position = toClipSpace(quad[vertexIndex]);
  return out;
}

fn signedDistanceToLine(point : vec2f, a : vec2f, b : vec2f) -> f32 {
  let ab = b - a;
  let t = clamp(dot(point - a, ab) / dot(ab, ab), 0.0, 1.0);
  let closest = a + ab * t;
  let perp = vec2f(-ab.y, ab.x);
  let sign = sign(dot(point - closest, perp));
  return length(point - closest) * sign;
}

fn evalQuadratic(p0 : vec2f, p1 : vec2f, p2 : vec2f, t : f32) -> vec2f {
  let u = 1.0 - t;
  return u * u * p0 + 2.0 * u * t * p1 + t * t * p2;
}

fn signedDistanceToQuadratic(point : vec2f, p0 : vec2f, p1 : vec2f, p2 : vec2f) -> f32 {
  // Iterative closest-point search adapted for WGSL; sufficient for geode AA smoothstep.
  var t = clamp(dot(point - p0, p2 - p0) / dot(p2 - p0, p2 - p0), 0.0, 1.0);
  var i = 0u;
  loop {
    let pos = evalQuadratic(p0, p1, p2, t);
    let tangent = normalize(2.0 * (p1 - p0) * (1.0 - t) + 2.0 * (p2 - p1) * t);
    let normal = vec2f(-tangent.y, tangent.x);
    let toPoint = point - pos;
    let d1 =
      2.0 * dot(pos - point, (p0 - p1) * (1.0 - t) + (p2 - p1) * t);
    let curvature = (p0 - p1) * (1.0 - t) + (p2 - p1) * t;
    let d2 = 2.0 * dot(curvature, curvature) + 2.0 * dot(pos - point, p0 - 2.0 * p1 + p2);
    if (abs(d2) > 1e-5) {
      t = clamp(t - d1 / d2, 0.0, 1.0);
    }
    i = i + 1u;
    if (i >= 5u) {
      break;
    }
  }

  let pos = evalQuadratic(p0, p1, p2, t);
  let tangent = normalize(2.0 * (p1 - p0) * (1.0 - t) + 2.0 * (p2 - p1) * t);
  let normal = vec2f(-tangent.y, tangent.x);
  let signFactor = sign(dot(point - pos, normal));
  return length(point - pos) * signFactor;
}

fn coverageAtPixel(position : vec2f) -> f32 {
  var dist = 1e6;

  for (var i = 0u; i < frame.segmentCount; i = i + 1u) {
    let seg = segments[i];
    if (seg.kind == 0u) {
      dist = min(dist, signedDistanceToLine(position, seg.p0, seg.p1));
    } else {
      dist = min(dist, signedDistanceToQuadratic(position, seg.p0, seg.p1, seg.p2));
    }
  }

  let aaWidth = 1.0; // pixel-based AA; refined later with dynamic derivatives.
  let coverage = clamp(0.5 - dist / aaWidth, 0.0, 1.0);
  return coverage;
}

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4f {
  let alpha = coverageAtPixel(in.localPosition);
  let color = vec3f(0.12, 0.63, 0.35);
  return vec4f(color * alpha, alpha);
}
