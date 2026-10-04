const kThird: f32 = 1f / 3f;
const kMixed = 1.0 / 3f;
const kCancelled = -1f + 1f;
@group(0) @binding(0) var outputTexture: texture_storage_2d<rgba32float, write>;
fn weights() -> vec2<f32> {
  return vec2<f32>(1f, 2f) / 3f + 2f * vec2<f32>(kThird, -0f - 0f);
}
@compute @workgroup_size(1, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
  let b = 1f / 3f;
  let scale = (12f - 9f * b) / 6f;
  textureStore(outputTexture, vec2i(gid.xy), vec4f(weights(), scale, kMixed + kCancelled));
}
