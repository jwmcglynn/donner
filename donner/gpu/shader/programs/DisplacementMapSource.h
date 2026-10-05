#pragma once
/// @file
/// Authoritative WGSL for feDisplacementMap.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
/// Authoritative WGSL for feDisplacementMap.
inline constexpr wgsl::SourceText kDisplacementMapSource{R"wgsl(struct DisplacementParams {
  scale: f32,
  xChannel: u32,
  yChannel: u32,
  padding: u32,
}

@group(0) @binding(0) var sourceTexture: texture_2d<f32>;
@group(0) @binding(1) var mapTexture: texture_2d<f32>;
@group(0) @binding(2) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(3) var<uniform> params: DisplacementParams;

fn channel(color: vec4<f32>, selector: u32) -> f32 {
  if ((selector == 3u)) {
    return color.w;
  }
  if ((color.w <= 0f)) {
    return 0f;
  }
  var value: f32 = color.w;
  if ((selector == 0u)) {
    value = color.x;
  }
  if ((selector == 1u)) {
    value = color.y;
  }
  if ((selector == 2u)) {
    value = color.z;
  }
  return min(1f, (value * (1f / color.w)));
}

fn sampleSource(coord: vec2<i32>, size: vec2<i32>) -> vec4<f32> {
  if ((any((coord < vec2<i32>(0i))) || any((coord >= size)))) {
    return vec4<f32>(0f);
  }
  return textureLoad(sourceTexture, coord, 0i);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let coord = vec2<i32>(gid.xy);
  let size = vec2<i32>(textureDimensions(outputTexture));
  if (any((coord >= size))) {
    return;
  }
  let map = textureLoad(mapTexture, coord, 0i);
  let position = (vec2<f32>(coord) + ((vec2<f32>(channel(map, params.xChannel), channel(map, params.yChannel)) - vec2<f32>(0.5f)) * params.scale));
  let base = vec2<i32>(floor(position));
  let fraction = (position - vec2<f32>(base));
  let top = (sampleSource((base + vec2<i32>(0i, 0i)), size) + (fraction.x * (sampleSource((base + vec2<i32>(1i, 0i)), size) - sampleSource((base + vec2<i32>(0i, 0i)), size))));
  let bottom = (sampleSource((base + vec2<i32>(0i, 1i)), size) + (fraction.x * (sampleSource((base + vec2<i32>(1i, 1i)), size) - sampleSource((base + vec2<i32>(0i, 1i)), size))));
  textureStore(outputTexture, coord, clamp((top + (fraction.y * (bottom - top))), vec4<f32>(0f), vec4<f32>(1f)));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
