#pragma once
/// @file
/// Authored WGSL source for the Gaussian and box blur compute program.

#include "donner/gpu/shader/wgsl/Compiler.h"

namespace donner::gpu::shader::programs {

/// Authored WGSL source for the Gaussian and box blur compute program.
inline constexpr wgsl::SourceText kGaussianBlurSource = R"wgsl(
struct BlurParams {
  stdDeviation: f32,
  axis: u32,
  edgeMode: u32,
  kernelType: u32,
  boxLeft: i32,
  boxRight: i32,
  clipMin: vec2<i32>,
  clipMax: vec2<i32>,
  clipActive: u32,
  pad: u32,
}

@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> params: BlurParams;

fn applyClip(coord: vec2<i32>, value: vec4<f32>) -> vec4<f32> {
  if ((params.clipActive == 1u)) {
    if ((any((coord < params.clipMin)) || any((coord >= params.clipMax)))) {
      return vec4<f32>(0f, 0f, 0f, 0f);
    }
  }
  return clamp(value, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
}

fn sampleEdge(coord: vec2<i32>, size: vec2<i32>) -> vec4<f32> {
  if ((params.edgeMode == 0u)) {
    if ((any((coord < vec2<i32>(0i, 0i))) || any((coord >= size)))) {
      return vec4<f32>(0f, 0f, 0f, 0f);
    }
    return textureLoad(inputTexture, coord, 0i);
  }
  if ((params.edgeMode == 1u)) {
    let clamped = clamp(coord, vec2<i32>(0i, 0i), (size - vec2<i32>(1i, 1i)));
    return textureLoad(inputTexture, clamped, 0i);
  }
  let wrapped = vec2<i32>((((coord.x % size.x) + size.x) % size.x), (((coord.y % size.y) + size.y) % size.y));
  return textureLoad(inputTexture, wrapped, 0i);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coord = vec2<i32>(gid.xy);
  let size = vec2<i32>(textureDimensions(inputTexture));
  if ((params.kernelType == 1u)) {
    var boxSum: vec4<f32> = vec4<f32>(0f, 0f, 0f, 0f);
    for (var boxIndex: i32 = (-params.boxLeft); (boxIndex <= params.boxRight); boxIndex = (boxIndex + 1i)) {
      boxSum = (boxSum + sampleEdge((coord + select(vec2<i32>(0i, boxIndex), vec2<i32>(boxIndex, 0i), (params.axis == 0u))), size));
    }
    textureStore(outputTexture, coord, applyClip(coord, (boxSum / f32(((params.boxLeft + params.boxRight) + 1i)))));
    return;
  }
  if ((params.stdDeviation <= 0f)) {
    textureStore(outputTexture, coord, applyClip(coord, textureLoad(inputTexture, coord, 0i)));
    return;
  }
  let radius = min(i32(ceil((3f * params.stdDeviation))), 127i);
  let inverseVariance = (1f / ((2f * params.stdDeviation) * params.stdDeviation));
  var sum: vec4<f32> = vec4<f32>(0f, 0f, 0f, 0f);
  var weightSum: f32 = 0f;
  for (var index: i32 = (-radius); (index <= radius); index = (index + 1i)) {
    let distance = f32(index);
    let weight = exp((((-distance) * distance) * inverseVariance));
    sum = (sum + (sampleEdge((coord + select(vec2<i32>(0i, index), vec2<i32>(index, 0i), (params.axis == 0u))), size) * weight));
    weightSum = (weightSum + weight);
  }
  if ((weightSum > 0f)) {
    sum = (sum / weightSum);
  }
  textureStore(outputTexture, coord, applyClip(coord, sum));
}
)wgsl";

}  // namespace donner::gpu::shader::programs
