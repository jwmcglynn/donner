#pragma once
/// @file
/// Authoritative WGSL for feFlood.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
inline constexpr wgsl::SourceText kFloodSource{R"wgsl(struct FloodParams {
  color: vec4<f32>,
}

@group(0) @binding(0) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(1) var<uniform> params: FloodParams;

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coords = vec2<i32>(gid.xy);
  textureStore(outputTexture, coords, params.color);
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
