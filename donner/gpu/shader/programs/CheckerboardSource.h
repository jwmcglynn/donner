#pragma once
/// @file
/// Authoritative WGSL for the transparency checkerboard.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
/// Authoritative WGSL for the transparency checkerboard.
inline constexpr wgsl::SourceText kCheckerboardSource{R"wgsl(struct CheckerboardParams {
  target_size: vec2<f32>,
  device_pixel_ratio: f32,
  checker_size: f32,
  dark_color: vec4<f32>,
  light_color: vec4<f32>,
  origin_offset: vec2<f32>,
  padding: vec2<f32>,
}

@group(0) @binding(0) var<uniform> params: CheckerboardParams;

struct vs_main_Output {
  @builtin(position) position: vec4<f32>,
}

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> vs_main_Output {
  return vs_main_Output(vec4<f32>(select(-1f, 3f, (vertex_index == 1u)), select(-1f, 3f, (vertex_index == 2u)), 0f, 1f));
}

struct fs_main_Output {
  @location(0) color: vec4<f32>,
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> fs_main_Output {
  let anchored = (min(position.xy, params.target_size) + params.origin_offset);
  let screen = (anchored / max(params.device_pixel_ratio, 1e-04f));
  let cell = vec2<i32>(floor((screen / vec2<f32>(params.checker_size, params.checker_size))));
  if ((((cell.x + cell.y) % 2i) == 0i)) {
    return fs_main_Output(params.light_color);
  }
  return fs_main_Output(params.dark_color);
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
