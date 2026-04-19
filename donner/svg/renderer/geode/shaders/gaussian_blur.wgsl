// Geode Gaussian-blur compute pipeline: two-pass separable Gaussian.
//
// Pass 1 blurs horizontally (axis=0), pass 2 blurs vertically (axis=1).
// Each pass reads from a texture_2d and writes to a texture_storage_2d.
//
// Kernel radius per pass = ceil(3 * stdDeviation). For stdDev >= 4 the
// kernel width exceeds 25 — a future optimisation can swap in a three-box
// approximation, but for the initial Phase 7 scaffolding the plain Gaussian
// is sufficient and simpler to validate.

struct BlurParams {
  std_deviation: f32,
  axis: u32,       // 0 = horizontal, 1 = vertical
  edge_mode: u32,  // 0 = None (transparent), 1 = Duplicate (clamp), 2 = Wrap
  _pad: u32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: BlurParams;

// Sample the input texture with edge-mode handling.
fn sampleEdge(coord: vec2i, size: vec2i) -> vec4f {
  if (params.edge_mode == 0u) {
    // None: out-of-bounds → transparent black.
    if (any(coord < vec2i(0)) || any(coord >= size)) {
      return vec4f(0.0);
    }
    return textureLoad(input_tex, coord, 0);
  } else if (params.edge_mode == 1u) {
    // Duplicate: clamp to nearest edge pixel.
    let clamped = clamp(coord, vec2i(0), size - vec2i(1));
    return textureLoad(input_tex, clamped, 0);
  } else {
    // Wrap: modular arithmetic.
    let wrapped = ((coord % size) + size) % size;
    return textureLoad(input_tex, wrapped, 0);
  }
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let sigma = params.std_deviation;

  // stdDeviation == 0 → passthrough (no blur).
  if (sigma <= 0.0) {
    textureStore(output_tex, coord, textureLoad(input_tex, coord, 0));
    return;
  }

  // Kernel radius: ceil(3 * sigma), capped at 127 to keep workgroup
  // memory pressure bounded.
  let radius = min(i32(ceil(3.0 * sigma)), 127);

  // Precompute 1 / (2 * sigma^2) for the Gaussian weight.
  let inv2sigma2 = 1.0 / (2.0 * sigma * sigma);

  var acc = vec4f(0.0);
  var weight_sum: f32 = 0.0;

  for (var i: i32 = -radius; i <= radius; i = i + 1) {
    let offset = select(vec2i(0, i), vec2i(i, 0), params.axis == 0u);
    let sample_coord = coord + offset;

    let dist = f32(i);
    let w = exp(-dist * dist * inv2sigma2);

    acc = acc + sampleEdge(sample_coord, size) * w;
    weight_sum = weight_sum + w;
  }

  if (weight_sum > 0.0) {
    acc = acc / weight_sum;
  }

  textureStore(output_tex, coord, acc);
}
