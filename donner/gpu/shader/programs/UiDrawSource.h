#pragma once
/// @file
/// Authoritative WGSL for UI draw-data rendering.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
/// Authoritative WGSL for UI draw-data rendering.
inline constexpr wgsl::SourceText kUiDrawSource{R"wgsl(struct UiDrawParams {
  clip_from_logical: mat4x4<f32>,
}

@group(0) @binding(0) var<uniform> params: UiDrawParams;
@group(0) @binding(1) var uiSampler: sampler;
@group(0) @binding(2) var uiTexture: texture_2d<f32>;

struct vs_main_Output {
  @builtin(position) position: vec4<f32>,
  @location(0) color: vec4<f32>,
  @location(1) uv: vec2<f32>,
}

// The vertex color arrives as one packed u32 because the runtime's vertex formats are float or
// u32 only. Unpacking uses division and masking: the shader profile has no shift operator.
@vertex
fn vs_main(@location(0) position: vec2<f32>, @location(1) uv: vec2<f32>, @location(2) packed_color: u32) -> vs_main_Output {
  let red = (packed_color & 255u);
  let green = ((packed_color / 256u) & 255u);
  let blue = ((packed_color / 65536u) & 255u);
  let alpha = ((packed_color / 16777216u) & 255u);
  let channels = vec4<f32>(f32(red), f32(green), f32(blue), f32(alpha));
  let color = (channels / vec4<f32>(255f, 255f, 255f, 255f));
  let clip_position = (params.clip_from_logical * vec4<f32>(position.x, position.y, 0f, 1f));
  return vs_main_Output(clip_position, color, uv);
}

struct fs_main_Output {
  @location(0) color: vec4<f32>,
}

@fragment
fn fs_straight_alpha(@location(0) color: vec4<f32>, @location(1) uv: vec2<f32>) -> fs_main_Output {
  return fs_main_Output((color * textureSample(uiTexture, uiSampler, uv)));
}

// A premultiplied source has its color channels already scaled by its own alpha, so the vertex
// tint scales them by the vertex alpha only. Multiplying by the sampled alpha again would darken
// every translucent pixel a second time.
@fragment
fn fs_premultiplied_alpha(@location(0) color: vec4<f32>, @location(1) uv: vec2<f32>) -> fs_main_Output {
  let texel = textureSample(uiTexture, uiSampler, uv);
  let tinted = ((texel.xyz * color.xyz) * vec3<f32>(color.w, color.w, color.w));
  return fs_main_Output(vec4<f32>(tinted.x, tinted.y, tinted.z, (texel.w * color.w)));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
