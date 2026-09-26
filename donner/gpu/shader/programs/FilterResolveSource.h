#pragma once
/// @file
/// Authoritative WGSL for clipping and resolving float filter output to RGBA8.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
/// Authoritative WGSL for clipping and resolving float filter output to RGBA8.
inline constexpr wgsl::SourceText kFilterResolveSource{R"wgsl(const kTransferCount: u32 = 8192u;

struct SubregionClipParams {
  invA: f32,
  invB: f32,
  invC: f32,
  invD: f32,
  invE: f32,
  invF: f32,
  userX0: f32,
  userY0: f32,
  userX1: f32,
  userY1: f32,
  pad0: u32,
  pad1: u32,
}

struct ColorTransferTable {
  samples: array<f32, kTransferCount>,
}

@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: SubregionClipParams;
@group(0) @binding(3) var<storage, read> transferTable: ColorTransferTable;

fn srgb_channel_to_linear(c: f32) -> f32 {
  var unit: f32 = 0f;
  if ((c > 0f)) {
    unit = min(c, 1f);
  }
  let scaled = (unit * 4095f);
  let index = u32((scaled + 0.5f));
  return transferTable.samples[(index + 0u)];
}

fn linear_channel_to_srgb(c: f32) -> f32 {
  var unit: f32 = 0f;
  if ((c > 0f)) {
    unit = min(c, 1f);
  }
  let scaled = (unit * 4095f);
  let index = u32((scaled + 0.5f));
  return transferTable.samples[(index + 4096u)];
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coords = vec2<i32>(gid.xy);
  let centerX = (f32(coords.x) + 0.5f);
  let centerY = (f32(coords.y) + 0.5f);
  let userX = (((params.invA * centerX) + (params.invC * centerY)) + params.invE);
  let userY = (((params.invB * centerX) + (params.invD * centerY)) + params.invF);
  let outside = ((((userX < params.userX0) || (userX >= params.userX1)) || (userY < params.userY0)) || (userY >= params.userY1));
  if (outside) {
    textureStore(outputTexture, coords, vec4<f32>(0f, 0f, 0f, 0f));
  } else {
    var resolvedColor: vec4<f32> = textureLoad(inputTexture, coords, 0i);
    if ((params.pad0 != 0u)) {
      let alpha = resolvedColor.w;
      if ((alpha > 0f)) {
        let straight = (resolvedColor.xyz * (1f / alpha));
        resolvedColor = vec4<f32>((vec3<f32>(linear_channel_to_srgb(straight.x), linear_channel_to_srgb(straight.y), linear_channel_to_srgb(straight.z)) * alpha), alpha);
      } else {
        resolvedColor = vec4<f32>(0f);
      }
    }
    textureStore(outputTexture, coords, (floor(((saturate(resolvedColor) * 255f) + vec4<f32>(0.5f))) / 255f));
  }
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
