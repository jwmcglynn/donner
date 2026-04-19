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

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(in1_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  // Read the displacement map at this pixel.
  let disp = textureLoad(in2_tex, coord, 0);

  // Compute the displacement offset.
  let cx = selectChannel(disp, params.x_channel);
  let cy = selectChannel(disp, params.y_channel);

  let dx = params.scale * (cx - 0.5);
  let dy = params.scale * (cy - 0.5);

  // Sample source at displaced position (nearest-neighbour).
  let src_x = i32(round(f32(coord.x) + dx));
  let src_y = i32(round(f32(coord.y) + dy));

  var result = vec4f(0.0);
  if (src_x >= 0 && src_x < size.x && src_y >= 0 && src_y < size.y) {
    result = textureLoad(in1_tex, vec2i(src_x, src_y), 0);
  }

  textureStore(output_tex, coord, result);
}
