// Geode feTile compute pipeline: tile an input subregion across the output.
//
// Per SVG Filter Effects §15.28, feTile replicates the input's primitive-
// subregion rectangle across the filter region with wraparound. The source
// rectangle is passed in as (src_x, src_y, src_w, src_h) in pixel-space.
// Output pixels outside the filter region are left unchanged by the caller;
// this shader unconditionally fills every output pixel with a tiled sample.

struct TileParams {
  src_x: i32,
  src_y: i32,
  src_w: i32,
  src_h: i32,
}

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: TileParams;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(output_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  // Degenerate source rect: passthrough-transparent.
  if (params.src_w <= 0 || params.src_h <= 0) {
    textureStore(output_tex, coord, vec4f(0.0));
    return;
  }

  // Map the output pixel into source coordinates by subtracting the source
  // origin and wrapping modulo the source dimensions. Both the C rem and
  // WGSL `%` follow truncated division, so negative values need `+ m) % m`
  // to end up in [0, m).
  let rel = coord - vec2i(params.src_x, params.src_y);
  let wrap = ((rel % vec2i(params.src_w, params.src_h)) +
              vec2i(params.src_w, params.src_h)) %
             vec2i(params.src_w, params.src_h);
  let sample_coord = wrap + vec2i(params.src_x, params.src_y);

  // Clamp the final sample coord back into the texture bounds. The source
  // rect may extend past the input texture if the caller passed a subregion
  // larger than what was rendered; clamping avoids UB on textureLoad.
  let clamped = clamp(sample_coord, vec2i(0), size - vec2i(1));
  textureStore(output_tex, coord, textureLoad(input_tex, clamped, 0));
}
