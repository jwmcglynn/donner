#pragma once
/// @file
/// Authoritative WGSL for feComponentTransfer.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
inline constexpr wgsl::SourceText kComponentTransferSource{
    R"wgsl(@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<storage, read> params: array<f32>;

fn transfer(value: f32, channel: u32) -> f32 {
  let c = clamp(value, 0f, 1f);
  let base = (channel * 8u);
  var result: f32 = c;
  if ((u32(params[(base + 0u)]) == 1u)) {
    if ((u32(params[(base + 2u)]) == 1u)) {
      result = params[(32u + u32(params[(base + 1u)]))];
    } else {
      if ((u32(params[(base + 2u)]) > 1u)) {
        let position = (c * f32((u32(params[(base + 2u)]) - 1u)));
        let index = min(u32(position), (u32(params[(base + 2u)]) - 2u));
        let fraction = (position - f32(index));
        result = ((params[(32u + (u32(params[(base + 1u)]) + index))] * (1f - fraction)) + (params[(32u + ((u32(params[(base + 1u)]) + index) + 1u))] * fraction));
      }
    }
  }
  if (((u32(params[(base + 0u)]) == 2u) && (u32(params[(base + 2u)]) > 0u))) {
    let discreteIndex = min(u32((c * f32(u32(params[(base + 2u)])))), (u32(params[(base + 2u)]) - 1u));
    result = params[(32u + (u32(params[(base + 1u)]) + discreteIndex))];
  }
  if ((u32(params[(base + 0u)]) == 3u)) {
    result = ((params[(base + 3u)] * c) + params[(base + 4u)]);
  }
  if ((u32(params[(base + 0u)]) == 4u)) {
    if ((params[(base + 5u)] == 0f)) {
      result = params[(base + 7u)];
    } else {
      if ((params[(base + 6u)] == 0f)) {
        result = (params[(base + 5u)] + params[(base + 7u)]);
      } else {
        if (((c == 0f) && (params[(base + 6u)] < 0f))) {
          result = select(0f, 1f, (params[(base + 5u)] > 0f));
        } else {
          result = ((params[(base + 5u)] * pow(c, params[(base + 6u)])) + params[(base + 7u)]);
        }
      }
    }
  }
  return clamp(result, 0f, 1f);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coord = vec2<i32>(gid.xy);
  let color = textureLoad(inputTexture, coord, 0i);
  var straight: vec4<f32> = vec4<f32>(0f);
  if ((color.w > 0f)) {
    straight = vec4<f32>((color.xyz / color.w), color.w);
  }
  let transformed = vec4<f32>(transfer(straight.x, 0u), transfer(straight.y, 1u), transfer(straight.z, 2u), transfer(straight.w, 3u));
  textureStore(outputTexture, coord, vec4<f32>((transformed.xyz * transformed.w), transformed.w));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
