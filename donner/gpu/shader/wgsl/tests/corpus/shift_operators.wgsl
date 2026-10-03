const kFlags = (1 << 4) - 1;
const kShift: u32 = 3u;
@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<storage, read> packed: array<vec2<u32>>;
fn unpack(word: u32) -> vec4<f32> {
  let channels = vec4<u32>(word & 255u, (word >> 8u) & 255u, (word >> 16u) & 255u, word >> 24u);
  return vec4<f32>(channels) / vec4<f32>(255.0);
}
fn mix_bits(value: vec2<i32>, amount: u32) -> vec2<i32> {
  var result = value;
  result <<= vec2<u32>(amount, kShift);
  result >>= vec2<u32>(1u);
  return result;
}
@compute @workgroup_size(1, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
  let texel = textureLoad(inputTexture, vec2i(gid.xy), 0);
  let amount = u32(texel.x);
  let word = packed[gid.x].x << (amount & 7u);
  let lanes = mix_bits(vec2<i32>(i32(texel.y), kFlags), amount);
  let below = (word >> kShift) < packed[gid.x].y;
  textureStore(outputTexture, vec2i(gid.xy),
               unpack(word) + vec4f(f32(lanes.x), f32(lanes.y), select(0.0, 1.0, below), 0.0));
}
