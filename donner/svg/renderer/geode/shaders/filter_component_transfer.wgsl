// Geode feComponentTransfer compute pipeline: per-channel LUT transform.
//
// The CPU pre-computes four 256-entry uint8 LUTs (R, G, B, A) and uploads
// them as a single 1024-byte storage buffer.  The shader un-premultiplies
// the input (feComponentTransfer is defined on straight-alpha per SVG 2
// §15.11), applies the LUT to each channel, then re-premultiplies.

struct ComponentTransferParams {
  // 256 entries × 4 channels = 1024 u32 values.
  // Layout: lut[0..255] = R, lut[256..511] = G, lut[512..767] = B,
  //         lut[768..1023] = A.
  // Each entry stores a uint8 value (0..255) in the low byte of a u32.
  lut: array<u32, 1024>,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<storage, read> params: ComponentTransferParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let c = textureLoad(input_tex, coord, 0);

  // Un-premultiply: divide RGB by alpha. Guard against div-by-zero.
  var straight: vec4f;
  if (c.a > 0.0) {
    straight = vec4f(c.rgb / c.a, c.a);
  } else {
    straight = vec4f(0.0);
  }

  // Quantise to 0..255 indices.
  let ri = clamp(u32(round(straight.r * 255.0)), 0u, 255u);
  let gi = clamp(u32(round(straight.g * 255.0)), 0u, 255u);
  let bi = clamp(u32(round(straight.b * 255.0)), 0u, 255u);
  let ai = clamp(u32(round(straight.a * 255.0)), 0u, 255u);

  // Look up each channel's transfer function result.
  let r_out = f32(params.lut[ri])       / 255.0;
  let g_out = f32(params.lut[256u + gi]) / 255.0;
  let b_out = f32(params.lut[512u + bi]) / 255.0;
  let a_out = f32(params.lut[768u + ai]) / 255.0;

  // Re-premultiply.
  let result = vec4f(r_out * a_out, g_out * a_out, b_out * a_out, a_out);

  textureStore(output_tex, coord, clamp(result, vec4f(0.0), vec4f(1.0)));
}
