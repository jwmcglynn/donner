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

fn round_half_away_from_zero(x: f32) -> f32 {
  let magnitude = abs(x);
  let integral = floor(magnitude);
  let direction = sign(x);
  if (((magnitude - integral) >= 0.5f)) {
    return (direction * (integral + 1f));
  }
  return (direction * integral);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coord = vec2<i32>(gid.xy);
  let sampleCoord = (coord - vec2<i32>(i32(round_half_away_from_zero(params.dx)), i32(round_half_away_from_zero(params.dy))));
  let blurredSize = vec2<i32>(textureDimensions(blurredTexture));
  var blurredAlpha: f32 = 0f;
  if ((all((sampleCoord >= vec2<i32>(0i))) && all((sampleCoord < blurredSize)))) {
    blurredAlpha = textureLoad(blurredTexture, sampleCoord, 0i).w;
  }
  let shadow = vec4<f32>(((params.color.xyz * params.color.w) * blurredAlpha), (params.color.w * blurredAlpha));
  let top = textureLoad(sourceTexture, coord, 0i);
  let result = (top + (shadow * (1f - top.w)));
  textureStore(outputTexture, coord, clamp(result, vec4<f32>(0f), vec4<f32>(1f)));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
