// Geode feFlood compute pipeline: fill every pixel with a constant color.
//
// No input texture — the output is entirely determined by the flood-color
// and flood-opacity uniforms.

struct FloodParams {
  color: vec4f,  // Pre-multiplied (r*a, g*a, b*a, a) or straight — caller decides.
}

@group(0) @binding(0) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(1) var<uniform> params: FloodParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(output_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  textureStore(output_tex, coord, params.color);
}
