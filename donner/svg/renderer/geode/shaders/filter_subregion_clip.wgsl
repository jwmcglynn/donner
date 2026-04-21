// Geode per-primitive subregion clipping compute shader.
//
// After each filter primitive dispatch, this shader clips the output to the
// primitive's subregion.  It transforms each pixel center to user space via
// the inverse CTM and tests against the axis-aligned user-space subregion
// rectangle.  Pixels outside the subregion are zeroed (transparent black);
// pixels inside are copied unchanged.
//
// This mirrors tiny-skia's applySubregionClipping() rotation-aware path.

struct SubregionClipParams {
  // Inverse transform (pixel → user space):
  //   u = inv_a * px + inv_c * py + inv_e
  //   v = inv_b * px + inv_d * py + inv_f
  inv_a: f32,
  inv_b: f32,
  inv_c: f32,
  inv_d: f32,
  inv_e: f32,
  inv_f: f32,
  // User-space subregion: [usr_x0, usr_y0) to [usr_x1, usr_y1).
  usr_x0: f32,
  usr_y0: f32,
  usr_x1: f32,
  usr_y1: f32,
  _pad0: u32,
  _pad1: u32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: SubregionClipParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(input_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  // Transform pixel center to user/filter space.
  let px = f32(coord.x) + 0.5;
  let py = f32(coord.y) + 0.5;
  let ux = params.inv_a * px + params.inv_c * py + params.inv_e;
  let uy = params.inv_b * px + params.inv_d * py + params.inv_f;

  if (ux < params.usr_x0 || ux >= params.usr_x1 ||
      uy < params.usr_y0 || uy >= params.usr_y1) {
    textureStore(output_tex, coord, vec4f(0.0));
  } else {
    textureStore(output_tex, coord, textureLoad(input_tex, coord, 0));
  }
}
