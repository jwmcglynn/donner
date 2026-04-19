// Geode feMerge alpha-over blit compute pipeline.
//
// Composites a source texture over a destination texture using the
// standard Porter-Duff source-over operator:
//   out = src + dst * (1 - src.a)
//
// feMerge is implemented by the CPU orchestrator as N sequential
// dispatches of this shader, one per feMergeNode child. The first
// dispatch reads the first input as "src" and an empty (transparent)
// texture as "dst". Subsequent dispatches read the next input as "src"
// and the previous output as "dst".

@group(0) @binding(0) var src_tex: texture_2d<f32>;
@group(0) @binding(1) var dst_tex: texture_2d<f32>;
@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = vec2i(textureDimensions(output_tex));
  let coord = vec2i(gid.xy);

  if (any(coord >= size)) {
    return;
  }

  let src = textureLoad(src_tex, coord, 0);
  let dst = textureLoad(dst_tex, coord, 0);

  // Porter-Duff source-over: out = src + dst * (1 - src.a).
  let out_color = src + dst * (1.0 - src.a);

  textureStore(output_tex, coord, clamp(out_color, vec4f(0.0), vec4f(1.0)));
}
