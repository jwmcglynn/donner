// Geode feConvolveMatrix compute pipeline: NxM kernel convolution.
//
// Applies a user-supplied convolution kernel to the input texture:
//   out.rgb = (sum(kernel[i,j] * in[x+i-tx, y+j-ty].rgb) / divisor) + bias
//   out.a   = preserveAlpha ? in[x,y].a : (kernel applied to alpha too)
//
// Edge modes:
//   0 = duplicate (clamp to nearest edge pixel)
//   1 = wrap (modular arithmetic)
//   2 = none (transparent black)
//
// Kernel is stored as an array of f32 in row-major order, max 5×5 = 25.

struct ConvolveParams {
  order_x: i32,       // Kernel width.
  order_y: i32,       // Kernel height.
  target_x: i32,      // Target pixel X offset.
  target_y: i32,      // Target pixel Y offset.
  divisor: f32,        // Divisor for the sum.
  bias: f32,           // Bias added after division.
  edge_mode: u32,      // 0 = duplicate, 1 = wrap, 2 = none.
  preserve_alpha: u32, // 1 = preserve alpha, 0 = convolve alpha too.
  kernel: array<f32, 25>,  // Row-major kernel values (max 5×5).
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<storage, read> params: ConvolveParams;

fn sampleEdge(p: vec2i, size: vec2i) -> vec4f {
  if (params.edge_mode == 0u) {
    // Duplicate: clamp to nearest edge pixel.
    let clamped = clamp(p, vec2i(0), size - vec2i(1));
    return textureLoad(input_tex, clamped, 0);
  } else if (params.edge_mode == 1u) {
    // Wrap: modular arithmetic.
    let wrapped = ((p % size) + size) % size;
    return textureLoad(input_tex, wrapped, 0);
  } else {
    // None: transparent black.
    if (any(p < vec2i(0)) || any(p >= size)) {
      return vec4f(0.0);
    }
    return textureLoad(input_tex, p, 0);
  }
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  var sum_rgb = vec3f(0.0);
  var sum_a: f32 = 0.0;

  for (var j = 0; j < params.order_y; j = j + 1) {
    for (var i = 0; i < params.order_x; i = i + 1) {
      let src = coord + vec2i(i - params.target_x, j - params.target_y);
      let sample = sampleEdge(src, size);
      let k = params.kernel[j * params.order_x + i];
      sum_rgb = sum_rgb + sample.rgb * k;
      sum_a = sum_a + sample.a * k;
    }
  }

  var result: vec4f;
  result = vec4f(sum_rgb / params.divisor + vec3f(params.bias),
                 sum_a / params.divisor + params.bias);

  if (params.preserve_alpha == 1u) {
    let original = textureLoad(input_tex, coord, 0);
    result.a = original.a;
  }

  textureStore(output_tex, coord, clamp(result, vec4f(0.0), vec4f(1.0)));
}
