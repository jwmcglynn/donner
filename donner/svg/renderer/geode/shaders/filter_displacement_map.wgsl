// Geode feDisplacementMap compute pipeline: per-pixel displacement.
//
// Implements the SVG feDisplacementMap primitive (Filter Effects Module Level 1
// §15.7). Each output pixel is sampled from the source image at a position
// offset by channel values read from the displacement map:
//
//   P'(x,y) = in1(x + scale*(Cx(x,y) - 0.5), y + scale*(Cy(x,y) - 0.5))
//
// where Cx and Cy are the channel selector values (R/G/B/A) from in2.
// Out-of-bounds samples return transparent black.

struct DisplacementParams {
  scale: f32,
  x_channel: u32,  // 0=R, 1=G, 2=B, 3=A
  y_channel: u32,  // 0=R, 1=G, 2=B, 3=A
  _pad: u32,
}

@group(0) @binding(0) var in1_tex: texture_2d<f32>;
@group(0) @binding(1) var in2_tex: texture_2d<f32>;
@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: DisplacementParams;

fn selectChannel(color: vec4f, channel: u32) -> f32 {
  switch (channel) {
    case 0u: { return color.r; }
    case 1u: { return color.g; }
    case 2u: { return color.b; }
    case 3u: { return color.a; }
    default: { return color.a; }
  }
}

// Sample in1 at integer (x, y), returning transparent black out of bounds.
fn sampleSrc(x: i32, y: i32, w: i32, h: i32) -> vec4f {
  if (x < 0 || y < 0 || x >= w || y >= h) {
    return vec4f(0.0);
  }
  return textureLoad(in1_tex, vec2i(x, y), 0);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(in1_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  // Read the displacement map at this pixel.
  let disp = textureLoad(in2_tex, coord, 0);

  // Un-premultiply the displacement channels: the spec reads the *color* (straight
  // alpha) channel values, and the textures here are premultiplied (matches tiny-
  // skia, which divides by alpha and clamps to 1.0). Alpha channel is used as-is.
  let inv_a = select(0.0, 1.0 / disp.a, disp.a > 0.0);
  let cx_raw = selectChannel(disp, params.x_channel);
  let cy_raw = selectChannel(disp, params.y_channel);
  let cx = select(min(1.0, cx_raw * inv_a), disp.a, params.x_channel == 3u);
  let cy = select(min(1.0, cy_raw * inv_a), disp.a, params.y_channel == 3u);

  let dx = params.scale * (cx - 0.5);
  let dy = params.scale * (cy - 0.5);

  // Sample source at displaced position with bilinear interpolation (matches
  // tiny-skia's FloatPixmap displacement). Out-of-bounds taps are transparent.
  let fx = f32(coord.x) + dx;
  let fy = f32(coord.y) + dy;
  let x0 = i32(floor(fx));
  let y0 = i32(floor(fy));
  let frac_x = fx - f32(x0);
  let frac_y = fy - f32(y0);

  let s00 = sampleSrc(x0,     y0,     size.x, size.y);
  let s10 = sampleSrc(x0 + 1, y0,     size.x, size.y);
  let s01 = sampleSrc(x0,     y0 + 1, size.x, size.y);
  let s11 = sampleSrc(x0 + 1, y0 + 1, size.x, size.y);

  let top = mix(s00, s10, frac_x);
  let bot = mix(s01, s11, frac_x);
  let result = clamp(mix(top, bot, frac_y), vec4f(0.0), vec4f(1.0));

  textureStore(output_tex, coord, result);
}
