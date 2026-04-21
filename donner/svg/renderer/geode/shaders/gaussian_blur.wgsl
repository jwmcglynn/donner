// Geode Gaussian-blur compute pipeline: two-pass separable Gaussian, with an
// optional box-blur mode used to build a 3-pass box approximation for larger
// sigma (matching tiny-skia's GaussianBlur for parity).
//
// Pass 1 blurs horizontally (axis=0), pass 2 blurs vertically (axis=1).
// Each pass reads from a texture_2d and writes to a texture_storage_2d.
//
// kernel_type == 0: discrete Gaussian, radius = ceil(3 * std_deviation).
// kernel_type == 1: box blur, taps span [-box_left, box_right] inclusive.
//                   Three sequential box passes per axis are mathematically
//                   equivalent (within ~0.1 sigma) to a Gaussian for sigma>=2,
//                   matching tiny-skia's effective extent (~2.82*sigma) instead
//                   of the wider 3*sigma cutoff used by the pure Gaussian path.

struct BlurParams {
  std_deviation: f32,
  axis: u32,       // 0 = horizontal, 1 = vertical
  edge_mode: u32,  // 0 = None (transparent), 1 = Duplicate (clamp), 2 = Wrap
  kernel_type: u32,// 0 = Gaussian, 1 = Box
  box_left: i32,   // box mode: number of samples to the negative side
  box_right: i32,  // box mode: number of samples to the positive side
  _pad0: u32,
  _pad1: u32,
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

  // Box-blur mode: average samples in [-box_left, box_right].
  if (params.kernel_type == 1u) {
    var bsum = vec4f(0.0);
    let bl = params.box_left;
    let br = params.box_right;
    for (var i: i32 = -bl; i <= br; i = i + 1) {
      let off = select(vec2i(0, i), vec2i(i, 0), params.axis == 0u);
      bsum = bsum + sampleEdge(coord + off, size);
    }
    let count = f32(bl + br + 1);
    textureStore(output_tex, coord, bsum / count);
    return;
  }

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
