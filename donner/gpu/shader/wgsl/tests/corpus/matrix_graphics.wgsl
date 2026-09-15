struct Params { mvp: mat4x4f, }
@group(0) @binding(0) var<uniform> params: Params;
fn axes(m: mat4x4f) -> mat2x2f {
  let x = (m * vec4f(1f, 0f, 0f, 1f)).xy;
  let y = (m * vec4f(0f, 1f, 0f, 1f)).xy;
  return mat2x2f(x, y);
}
fn det(m: mat2x2f) -> f32 { return m[0i].x * m[1i].y - m[0i].y * m[1i].x; }
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
  let matrix = axes(params.mvp);
  let v = matrix * vec2f(f32(index), 1f);
  return params.mvp * vec4f(v, det(matrix), 1f);
}
@fragment fn fs_main() -> @location(0) vec4f { return vec4f(1f); }
