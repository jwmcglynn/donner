@group(0) @binding(0) var outputTexture: texture_storage_2d<rgba32float, write>;
const kNegated = -(1.0 / 3.0);
fn keyword() -> f32 { return-1f*-3f; }
fn paren() -> f32 { return(1f)/3f; }
fn vector() -> vec2<f32> { return(vec2<f32>(1f, -2f))*2f; }
fn argument(x: f32) -> f32 { return max(x,1f-2f); }
fn negated() -> f32 { let b = -(1.0 / 3.0); return b + kNegated; }
fn spread() -> f32 {
  return 1f / // the divisor follows
    4f;
}
@compute @workgroup_size(1, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
  let total = keyword() + paren() + vector().y + argument(1f) + negated() + spread();
  textureStore(outputTexture, vec2i(gid.xy), vec4f(total));
}
