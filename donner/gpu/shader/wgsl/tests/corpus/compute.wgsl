struct Parameters { scale: f32, count: i32, }
@group(0) @binding(0) var image: texture_2d<f32>;
@group(0) @binding(1) var outputImage: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> parameters: Parameters;
fn sample(coord: vec2<i32>) -> vec4<f32> {
  return textureLoad(image, coord, 0i);
}
@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let coord = vec2<i32>(gid.xy);
  var color: vec4<f32> = vec4<f32>(0f);
  for (var i: i32 = 0i; i < parameters.count; i = i + 1i) {
    color = color + sample(coord) * parameters.scale;
  }
  textureStore(outputImage, coord, color);
}
