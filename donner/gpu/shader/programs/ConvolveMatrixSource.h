#pragma once
/// @file
/// Authored WGSL for SVG matrix convolution.

#include "donner/gpu/shader/wgsl/Compiler.h"

namespace donner::gpu::shader::programs {

/// Matrix convolution over a bounded coefficient array and sampled input image.
inline constexpr wgsl::SourceText kConvolveMatrixSource = R"wgsl(
struct ConvolveMatrixParams {
  orderX: i32,
  orderY: i32,
  targetX: i32,
  targetY: i32,
  divisor: f32,
  bias: f32,
  edgeMode: u32,
  preserveAlpha: u32,
  coefficients: array<f32, 25>,
}

@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<storage, read> params: ConvolveMatrixParams;

fn sampleEdge(coord: vec2<i32>, size: vec2<i32>) -> vec4<f32> {
  if ((params.edgeMode == 0u)) {
    let clamped = clamp(coord, vec2<i32>(0i), (size - vec2<i32>(1i)));
    return textureLoad(inputTexture, clamped, 0i);
  }
  if ((params.edgeMode == 1u)) {
    let wrapped = vec2<i32>((((coord.x % size.x) + size.x) % size.x), (((coord.y % size.y) + size.y) % size.y));
    return textureLoad(inputTexture, wrapped, 0i);
  }
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
  let coord = vec2<i32>(gid.xy);
  let size = vec2<i32>(textureDimensions(inputTexture));
  var sumRgb: vec3<f32> = vec3<f32>(0f);
  var sumAlpha: f32 = 0f;
  for (var j: i32 = 0i; (j < params.orderY); j = (j + 1i)) {
    for (var i: i32 = 0i; (i < params.orderX); i = (i + 1i)) {
      let sourceCoord = (coord + vec2<i32>((i - params.targetX), (j - params.targetY)));
      let source = sampleEdge(sourceCoord, size);
      let kernelIndex = ((((params.orderY - 1i) - j) * params.orderX) + ((params.orderX - 1i) - i));
      let coefficient = params.coefficients[clamp(kernelIndex, 0i, 24i)];
      if (((params.preserveAlpha == 1u) && (source.w > 0f))) {
        sumRgb = (sumRgb + ((source.xyz / source.w) * coefficient));
      } else {
        sumRgb = (sumRgb + (source.xyz * coefficient));
      }
      sumAlpha = (sumAlpha + (source.w * coefficient));
    }
  }
  let sourceAlpha = textureLoad(inputTexture, coord, 0i).w;
  var result: vec4<f32> = vec4<f32>(0f);
  if ((params.preserveAlpha == 1u)) {
    let straightRgb = clamp(((sumRgb / params.divisor) + vec3<f32>(params.bias)), vec3<f32>(0f), vec3<f32>(1f));
    result = vec4<f32>((straightRgb * sourceAlpha), sourceAlpha);
  } else {
    let biased = (params.bias * sourceAlpha);
    let outputAlpha = clamp(((sumAlpha / params.divisor) + biased), 0f, 1f);
    let outputRgb = clamp(((sumRgb / params.divisor) + vec3<f32>(biased)), vec3<f32>(0f), vec3<f32>(outputAlpha));
    result = vec4<f32>(outputRgb, outputAlpha);
  }
  textureStore(outputTexture, coord, clamp(result, vec4<f32>(0f), vec4<f32>(1f)));
}
)wgsl";

}  // namespace donner::gpu::shader::programs
