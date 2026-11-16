#pragma once
/// @file
/// Authoritative WGSL for the feDropShadow composition pass.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
/// Authoritative WGSL for the feDropShadow composition pass.
inline constexpr wgsl::SourceText kDropShadowSource{R"wgsl(struct DropShadowParams {
  color: vec4<f32>,
  dx: f32,
  dy: f32,
  pad0: u32,
  pad1: u32,
}

@group(0) @binding(0) var sourceTexture: texture_2d<f32>;
@group(0) @binding(1) var blurredTexture: texture_2d<f32>;
@group(0) @binding(2) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(3) var<uniform> params: DropShadowParams;

fn blurredAlphaAt(coord: vec2<i32>, size: vec2<i32>) -> f32 {
  if ((any((coord < vec2<i32>(0i))) || any((coord >= size)))) {
    return 0f;
  }
  return textureLoad(blurredTexture, coord, 0i).w;
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coord = vec2<i32>(gid.xy);
  let blurredSize = vec2<i32>(textureDimensions(blurredTexture));
  let position = (vec2<f32>(coord) - vec2<f32>(params.dx, params.dy));
  let base = vec2<i32>(floor(position));
  let fraction = (position - floor(position));
  let topLeft = blurredAlphaAt(base, blurredSize);
  let topRight = blurredAlphaAt((base + vec2<i32>(1i, 0i)), blurredSize);
  let bottomLeft = blurredAlphaAt((base + vec2<i32>(0i, 1i)), blurredSize);
  let bottomRight = blurredAlphaAt((base + vec2<i32>(1i, 1i)), blurredSize);
  let upperAlpha = (topLeft + (fraction.x * (topRight - topLeft)));
  let lowerAlpha = (bottomLeft + (fraction.x * (bottomRight - bottomLeft)));
  let blurredAlpha = (upperAlpha + (fraction.y * (lowerAlpha - upperAlpha)));
  let shadow = vec4<f32>(((params.color.xyz * params.color.w) * blurredAlpha), (params.color.w * blurredAlpha));
  let top = textureLoad(sourceTexture, coord, 0i);
  let result = (top + (shadow * (1f - top.w)));
  textureStore(outputTexture, coord, clamp(result, vec4<f32>(0f), vec4<f32>(1f)));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
