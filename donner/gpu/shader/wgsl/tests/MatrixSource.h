#pragma once
/// @file
/// Matrix transforms, value parameters, column indexing and reflected uniform layout.

#include "donner/gpu/shader/wgsl/Compiler.h"

namespace donner::gpu::shader::wgsl::tests {
/// A matrix graphics fixture shared by compiler and offline validation tests.
inline constexpr SourceText kMatrixSource{R"wgsl(
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
)wgsl"};
/// Non-square products, both scalar orders, vector-left products and zero/copy construction.
inline constexpr SourceText kMatrixOperationsSource{R"wgsl(
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
)wgsl"};

}  // namespace donner::gpu::shader::wgsl::tests
