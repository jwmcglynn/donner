struct Varyings {
  @builtin(position) position: vec4<f32>,
  @location(0) uv: vec2<f32>,
};
@vertex fn vs_main(@builtin(vertex_index) vertex_index: u32) -> Varyings {
  var point = vec2<f32>(-1f, -1f);
  if (vertex_index == 1u) { point = vec2<f32>(3f, -1f); }
  if (vertex_index == 2u) { point = vec2<f32>(-1f, 3f); }
  var output: Varyings;
  output.position = vec4<f32>(point, 0f, 1f);
  output.uv = (point + vec2<f32>(1f)) * vec2<f32>(0.5f);
  return output;
}
@fragment fn fs_main(input: Varyings) -> @location(0) vec4<f32> {
  return vec4<f32>(input.uv, 0f, 1f);
}
