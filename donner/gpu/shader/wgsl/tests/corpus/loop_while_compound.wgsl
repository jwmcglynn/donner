@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
fn evaluate(seed: f32) -> f32 {
  var total = 0.0;
  var steps = 8;
  var index = 0u;
  loop {
    if (index == 4u) { break; }
    var inner = 0u;
    while (inner < index) {
      total += seed;
      inner += 1u;
    }
    steps -= 1;
    index += 1u;
  }
  var scaled = total;
  scaled -= seed;
  return scaled + f32(steps) * 0.0625;
}
@compute @workgroup_size(1, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
  let v = textureLoad(inputTexture, vec2i(gid.xy), 0);
  textureStore(outputTexture, vec2i(gid.xy), vec4f(evaluate(v.x)));
}
