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

const kKernelBox: u32 = 1u;
const kKernelBoxCascade: u32 = 2u;
const kKernelDownsample: u32 = 3u;
const kKernelUpsample: u32 = 4u;
const kKernelQuadraticSpline: u32 = 5u;
const kEdgeTransparent: u32 = 0u;
const kEdgeDuplicate: u32 = 1u;

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
  if ((params.edgeMode == kEdgeTransparent)) {
    if ((any((coord < vec2<i32>(0i, 0i))) || any((coord >= size)))) {
      return vec4<f32>(0f, 0f, 0f, 0f);
    }
    return textureLoad(inputTexture, coord, 0i);
  }
  if ((params.edgeMode == kEdgeDuplicate)) {
    let clamped = clamp(coord, vec2<i32>(0i, 0i), (size - vec2<i32>(1i, 1i)));
    return textureLoad(inputTexture, clamped, 0i);
  }
  let wrapped = vec2<i32>((((coord.x % size.x) + size.x) % size.x), (((coord.y % size.y) + size.y) % size.y));
  return textureLoad(inputTexture, wrapped, 0i);
}

fn pairCount(index: i32, narrow: i32, span: i32) -> i32 {
  return max(0i, min(min((index + 1i), (span - index)), narrow));
}

fn pairCountPrefix(terms: i32, narrow: i32, wide: i32) -> i32 {
  let span = ((narrow + wide) - 1i);
  if ((terms <= 0i)) {
    return 0i;
  }
  if ((terms >= span)) {
    return (narrow * wide);
  }
  if ((terms < narrow)) {
    return ((terms * (terms + 1i)) / 2i);
  }
  if ((terms <= wide)) {
    return ((((narrow - 1i) * narrow) / 2i) + ((terms - (narrow - 1i)) * narrow));
  }
  let rest = (span - terms);
  return ((narrow * wide) - ((rest * (rest + 1i)) / 2i));
}

fn resampleFactor() -> vec2<i32> {
  return vec2<i32>((params.boxLeft % 256i), (params.boxRight % 256i));
}

fn resamplePhase() -> vec2<i32> {
  return vec2<i32>((params.boxLeft / 256i), (params.boxRight / 256i));
}

fn isResampleKernel() -> bool {
  return ((params.kernelType == kKernelDownsample) || (params.kernelType == kKernelUpsample));
}

fn downsampleBlock(coord: vec2<i32>, size: vec2<i32>) -> vec4<f32> {
  let factor = resampleFactor();
  let origin = ((coord * factor) - resamplePhase());
  var blockSum: vec4<f32> = vec4<f32>(0f, 0f, 0f, 0f);
  for (var row: i32 = 0i; (row < factor.y); row = (row + 1i)) {
    for (var column: i32 = 0i; (column < factor.x); column = (column + 1i)) {
      blockSum = (blockSum + sampleEdge((origin + vec2<i32>(column, row)), size));
    }
  }
  return (blockSum / f32((factor.x * factor.y)));
}

fn upsampleTap(position: i32, factor: i32) -> vec2<f32> {
  let residue = (((2i * (position % factor)) + 1i) - factor);
  let before = (residue < 0i);
  let base = ((position / factor) - select(0i, 1i, before));
  let weight = (f32(select(residue, (residue + (2i * factor)), before)) / f32((2i * factor)));
  return vec2<f32>(f32(base), weight);
}

fn upsampleBilinear(coord: vec2<i32>, size: vec2<i32>) -> vec4<f32> {
  let factor = resampleFactor();
  let position = (coord + resamplePhase());
  let tapX = upsampleTap(position.x, factor.x);
  let tapY = upsampleTap(position.y, factor.y);
  let limit = (size - vec2<i32>(1i, 1i));
  let base = vec2<i32>(i32(tapX.x), i32(tapY.x));
  let low = clamp(base, vec2<i32>(0i, 0i), limit);
  let high = clamp((base + vec2<i32>(1i, 1i)), vec2<i32>(0i, 0i), limit);
  let top = mix(textureLoad(inputTexture, low, 0i), textureLoad(inputTexture, vec2<i32>(high.x, low.y), 0i), tapX.y);
  let bottom = mix(textureLoad(inputTexture, vec2<i32>(low.x, high.y), 0i), textureLoad(inputTexture, high, 0i), tapX.y);
  return mix(top, bottom, tapY.y);
}

fn resample(coord: vec2<i32>, size: vec2<i32>) -> vec4<f32> {
  if ((params.kernelType == kKernelDownsample)) {
    return downsampleBlock(coord, size);
  }
  return upsampleBilinear(coord, size);
}

fn tapWeight(distance: f32, inverseVariance: f32, spline: bool) -> f32 {
  if (spline) {
    let t = (abs(distance) / params.stdDeviation);
    let outer = select(0f, ((0.5f * (1.5f - t)) * (1.5f - t)), (t < 1.5f));
    return select(outer, (0.75f - (t * t)), (t <= 0.5f));
  }
  return exp((((-distance) * distance) * inverseVariance));
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coord = vec2<i32>(gid.xy);
  let size = vec2<i32>(textureDimensions(inputTexture));
  if (isResampleKernel()) {
    textureStore(outputTexture, coord, applyClip(coord, resample(coord, size)));
    return;
  }
  if ((params.kernelType == kKernelBoxCascade)) {
    let half = (params.boxLeft / 2i);
    let even = ((params.boxLeft % 2i) == 0i);
    let firstRight = select(half, (half - 1i), even);
    let secondLeft = select(half, (half - 1i), even);
    let secondWidth = ((secondLeft + half) + 1i);
    let thirdWidth = ((half + half) + 1i);
    let narrow = min(secondWidth, thirdWidth);
    let wide = max(secondWidth, thirdWidth);
    let span = ((narrow + wide) - 1i);
    let lowestPair = (-(secondLeft + half));
    let scale = (1f / ((f32(((half + firstRight) + 1i)) * f32(secondWidth)) * f32(thirdWidth)));
    let horizontal = (params.axis == 0u);
    let along = select(coord.y, coord.x, horizontal);
    let lineLength = select(size.y, size.x, horizontal);
    let firstTap = max((-((half + secondLeft) + half)), (-along));
    let lastTap = min(((firstRight + half) + half), ((lineLength - 1i) - along));
    var count: i32 = (pairCountPrefix((((firstTap + half) - lowestPair) + 1i), narrow, wide) - pairCountPrefix(((firstTap - firstRight) - lowestPair), narrow, wide));
    var cascadeSum: vec4<f32> = vec4<f32>(0f, 0f, 0f, 0f);
    for (var tap: i32 = firstTap; (tap <= lastTap); tap = (tap + 1i)) {
      cascadeSum = (cascadeSum + (textureLoad(inputTexture, (coord + select(vec2<i32>(0i, tap), vec2<i32>(tap, 0i), horizontal)), 0i) * f32(count)));
      count = ((count + pairCount((((tap + half) + 1i) - lowestPair), narrow, span)) - pairCount(((tap - firstRight) - lowestPair), narrow, span));
    }
    textureStore(outputTexture, coord, applyClip(coord, (cascadeSum * scale)));
    return;
  }
  if ((params.kernelType == kKernelBox)) {
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
  let spline = (params.kernelType == kKernelQuadraticSpline);
  let radius = min(i32(ceil((select(3f, 1.5f, spline) * params.stdDeviation))), 127i);
  let inverseVariance = (1f / ((2f * params.stdDeviation) * params.stdDeviation));
  var sum: vec4<f32> = vec4<f32>(0f, 0f, 0f, 0f);
  var weightSum: f32 = 0f;
  for (var index: i32 = (-radius); (index <= radius); index = (index + 1i)) {
    let distance = f32(index);
    let weight = tapWeight(distance, inverseVariance, spline);
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
