// Geode snapshot readback: unpremultiply a render target on the GPU.
//
// Reads the premultiplied-alpha render target and writes straight-alpha RGBA8
// into a storage texture, replacing the CPU per-pixel unpremultiply loop in
// RendererGeode::ReadGeodeTextureSnapshot. The caller then copies the storage
// texture into a map-readable buffer.
//
// The integer arithmetic replicates the CPU reference formula byte-for-byte:
//
//   a == 0   -> (0, 0, 0, 0)
//   a != 0   -> min(255, (premul * 255 + a/2) / a)   (round-half-up)
//
// The recovered 8-bit channels are the unorm texel bytes, so the stored
// output matches the CPU path exactly; see the sourceAlphaType handling in
// RendererGeode.cc.
//
// Bind group:
// - binding(0) input_tex: texture_2d<f32> - premultiplied render target.
// - binding(1) output_tex: texture_storage_2d<rgba8unorm, write> - straight
//   RGBA8 staging texture, then copied into the readback buffer.

@group(0) @binding(0) var input_tex: texture_2d<f32>;
@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let size = textureDimensions(input_tex);
  if (any(gid.xy >= size)) {
    return;
  }

  let premul = textureLoad(input_tex, vec2i(gid.xy), 0);

  // Recover the stored 8-bit channels from the normalized unorm floats.
  let r8 = u32(round(premul.r * 255.0));
  let g8 = u32(round(premul.g * 255.0));
  let b8 = u32(round(premul.b * 255.0));
  let a8 = u32(round(premul.a * 255.0));

  var sr = 0u;
  var sg = 0u;
  var sb = 0u;
  if (a8 != 0u) {
    let half = a8 >> 1u;
    sr = min(255u, (r8 * 255u + half) / a8);
    sg = min(255u, (g8 * 255u + half) / a8);
    sb = min(255u, (b8 * 255u + half) / a8);
  }

  // Normalized floats round back to the same bytes on unorm storage.
  textureStore(output_tex, vec2i(gid.xy),
               vec4f(f32(sr) / 255.0, f32(sg) / 255.0, f32(sb) / 255.0, f32(a8) / 255.0));
}
