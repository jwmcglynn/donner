#pragma once
/// @file
/// Authoritative WGSL for SVG offset filtering; fractional offsets resample bilinearly.

#include "donner/gpu/shader/wgsl/Compiler.h"

namespace donner::gpu::shader::programs {
/// Authoritative WGSL for SVG offset filtering; fractional offsets resample bilinearly.
inline constexpr wgsl::SourceText kOffsetSource{R"wgsl(struct OffsetParams {
  dx: f32,
  dy: f32,
  pad0: u32,
  pad1: u32,
}

@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> params: OffsetParams;

fn sampleInput(coord: vec2<i32>, size: vec2<i32>) -> vec4<f32> {
  if ((any((coord < vec2<i32>(0i))) || any((coord >= size)))) {
    return vec4<f32>(0f);
  }
  return textureLoad(inputTexture, coord, 0i);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coords = vec2<i32>(gid.xy);
  let size = vec2<i32>(textureDimensions(inputTexture));
  let position = (vec2<f32>(coords) - vec2<f32>(params.dx, params.dy));
  let base = vec2<i32>(floor(position));
  let fraction = (position - floor(position));
  let topLeft = sampleInput(base, size);
  let topRight = sampleInput((base + vec2<i32>(1i, 0i)), size);
  let bottomLeft = sampleInput((base + vec2<i32>(0i, 1i)), size);
  let bottomRight = sampleInput((base + vec2<i32>(1i, 1i)), size);
  let top = (topLeft + (fraction.x * (topRight - topLeft)));
  let bottom = (bottomLeft + (fraction.x * (bottomRight - bottomLeft)));
  textureStore(outputTexture, coords, (top + (fraction.y * (bottom - top))));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
