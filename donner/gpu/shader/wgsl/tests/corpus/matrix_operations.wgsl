fn operations(p: vec4f) -> vec3f {
  let m = mat2x3f(vec3f(p.x, 0f, 1f), vec3f(0f, p.y, 1f));
  let n = mat4x2f(vec2f(1f, 0f), vec2f(0f, 1f), vec2f(1f, 1f), vec2f(0f, 0f));
  let product = m * n;
  let column = product * p;
  let row = column * m;
  let scaled = 0.5f * (m * 0.5f);
  let copied = mat2x3f(scaled);
  var zero: mat2x3f;
  let zero_constructor = mat2x3f();
  return copied * row + zero * row + zero_constructor * row;
}
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
  return vec4f(f32(index), 0f, 0f, 1f);
}
@fragment fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
  return vec4f(operations(position), 1f);
}
