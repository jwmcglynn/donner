#pragma once
/// @file
/// Authoritative WGSL for feImage placement.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
inline constexpr wgsl::SourceText kFilterImageSource{R"wgsl(struct ImageParams {
  m00: f32,
  m01: f32,
  m02: f32,
  m10: f32,
  m11: f32,
  m12: f32,
  samplingMode: u32,
  pixelatedScaleX: f32,
  pixelatedScaleY: f32,
  padding: u32,
}

@group(0) @binding(0) var imageTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> params: ImageParams;

fn sampleImage(coord: vec2<i32>, size: vec2<i32>) -> vec4<f32> {
  return textureLoad(imageTexture, clamp(coord, vec2<i32>(0i), (size - vec2<i32>(1i))), 0i);
}

fn sampleVirtualImage(coord: vec2<f32>, size: vec2<i32>, multiple: vec2<f32>) -> vec4<f32> {
  return sampleImage(clamp(vec2<i32>(floor((coord / multiple))), vec2<i32>(0i), (size - vec2<i32>(1i))), size);
}

fn samplePixelated(position: vec2<f32>, size: vec2<i32>, multiple: vec2<f32>) -> vec4<f32> {
  let virtualPosition = (((position + vec2<f32>(0.5f)) * multiple) - vec2<f32>(0.5f));
  let base = floor(virtualPosition);
  let fraction = (virtualPosition - base);
  let top = (sampleVirtualImage((base + vec2<f32>(0f, 0f)), size, multiple) + (fraction.x * (sampleVirtualImage((base + vec2<f32>(1f, 0f)), size, multiple) - sampleVirtualImage((base + vec2<f32>(0f, 0f)), size, multiple))));
  let bottom = (sampleVirtualImage((base + vec2<f32>(0f, 1f)), size, multiple) + (fraction.x * (sampleVirtualImage((base + vec2<f32>(1f, 1f)), size, multiple) - sampleVirtualImage((base + vec2<f32>(0f, 1f)), size, multiple))));
  return (top + (fraction.y * (bottom - top)));
}

fn cubicWeight(inputValue: f32) -> f32 {
  let distance = abs(inputValue);
  let b = 0.33333334f;
  if ((distance < 1f)) {
    return (((((((12f - (9f * b)) - (6f * b)) * ((distance * distance) * distance)) + (((-18f + (12f * b)) + (6f * b)) * (distance * distance))) + (0f * distance)) + (6f - (2f * b))) / 6f);
  }
  if ((distance < 2f)) {
    return (((((((-1f * b) - (6f * b)) * ((distance * distance) * distance)) + (((6f * b) + (30f * b)) * (distance * distance))) + (((-12f * b) - (48f * b)) * distance)) + ((8f * b) + (24f * b))) / 6f);
  }
  return 0f;
}

fn sampleSmooth(position: vec2<f32>, size: vec2<i32>) -> vec4<f32> {
  let base = vec2<i32>(floor(position));
  let weightX0 = cubicWeight((position.x - f32((base.x + -1i))));
  let weightY0 = cubicWeight((position.y - f32((base.y + -1i))));
  let weightX1 = cubicWeight((position.x - f32((base.x + 0i))));
  let weightY1 = cubicWeight((position.y - f32((base.y + 0i))));
  let weightX2 = cubicWeight((position.x - f32((base.x + 1i))));
  let weightY2 = cubicWeight((position.y - f32((base.y + 1i))));
  let weightX3 = cubicWeight((position.x - f32((base.x + 2i))));
  let weightY3 = cubicWeight((position.y - f32((base.y + 2i))));
  var accumulated: vec4<f32> = vec4<f32>(0f);
  var row0: vec4<f32> = vec4<f32>(0f);
  row0 = (row0 + (sampleImage((base + vec2<i32>(-1i, -1i)), size) * weightX0));
  row0 = (row0 + (sampleImage((base + vec2<i32>(0i, -1i)), size) * weightX1));
  row0 = (row0 + (sampleImage((base + vec2<i32>(1i, -1i)), size) * weightX2));
  row0 = (row0 + (sampleImage((base + vec2<i32>(2i, -1i)), size) * weightX3));
  accumulated = (accumulated + (row0 * weightY0));
  var row1: vec4<f32> = vec4<f32>(0f);
  row1 = (row1 + (sampleImage((base + vec2<i32>(-1i, 0i)), size) * weightX0));
  row1 = (row1 + (sampleImage((base + vec2<i32>(0i, 0i)), size) * weightX1));
  row1 = (row1 + (sampleImage((base + vec2<i32>(1i, 0i)), size) * weightX2));
  row1 = (row1 + (sampleImage((base + vec2<i32>(2i, 0i)), size) * weightX3));
  accumulated = (accumulated + (row1 * weightY1));
  var row2: vec4<f32> = vec4<f32>(0f);
  row2 = (row2 + (sampleImage((base + vec2<i32>(-1i, 1i)), size) * weightX0));
  row2 = (row2 + (sampleImage((base + vec2<i32>(0i, 1i)), size) * weightX1));
  row2 = (row2 + (sampleImage((base + vec2<i32>(1i, 1i)), size) * weightX2));
  row2 = (row2 + (sampleImage((base + vec2<i32>(2i, 1i)), size) * weightX3));
  accumulated = (accumulated + (row2 * weightY2));
  var row3: vec4<f32> = vec4<f32>(0f);
  row3 = (row3 + (sampleImage((base + vec2<i32>(-1i, 2i)), size) * weightX0));
  row3 = (row3 + (sampleImage((base + vec2<i32>(0i, 2i)), size) * weightX1));
  row3 = (row3 + (sampleImage((base + vec2<i32>(1i, 2i)), size) * weightX2));
  row3 = (row3 + (sampleImage((base + vec2<i32>(2i, 2i)), size) * weightX3));
  accumulated = (accumulated + (row3 * weightY3));
  return accumulated;
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let coord = vec2<i32>(gid.xy);
  let outputSize = vec2<i32>(textureDimensions(outputTexture));
  if (any((coord >= outputSize))) {
    return;
  }
  let imageSize = vec2<i32>(textureDimensions(imageTexture));
  if (any((imageSize <= vec2<i32>(0i)))) {
    textureStore(outputTexture, coord, vec4<f32>(0f));
    return;
  }
  let center = (vec2<f32>(coord) + vec2<f32>(0.5f));
  let position = vec2<f32>(((((params.m00 * center.x) + (params.m01 * center.y)) + params.m02) - 0.5f), ((((params.m10 * center.x) + (params.m11 * center.y)) + params.m12) - 0.5f));
  if ((any((position < vec2<f32>(-0.5f))) || any((position >= (vec2<f32>(imageSize) - vec2<f32>(0.5f)))))) {
    textureStore(outputTexture, coord, vec4<f32>(0f));
    return;
  }
  var sampled: vec4<f32> = vec4<f32>(0f);
  if ((params.samplingMode == 1u)) {
    sampled = sampleImage(vec2<i32>(floor((position + vec2<f32>(0.5f)))), imageSize);
  } else {
    if ((params.samplingMode == 2u)) {
      sampled = samplePixelated(position, imageSize, clamp(floor((vec2<f32>(params.pixelatedScaleX, params.pixelatedScaleY) + vec2<f32>(0.5f))), vec2<f32>(1f), vec2<f32>(65536f)));
    } else {
      sampled = sampleSmooth(position, imageSize);
    }
  }
  let clamped = clamp(sampled, vec4<f32>(0f), vec4<f32>(1f));
  textureStore(outputTexture, coord, vec4<f32>(min(clamped.xyz, vec3<f32>(clamped.w)), clamped.w));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
