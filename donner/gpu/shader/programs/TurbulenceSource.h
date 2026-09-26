#pragma once
/// @file
/// Authoritative WGSL for SVG Perlin turbulence and fractal noise.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
/// Authoritative WGSL for SVG Perlin turbulence and fractal noise.
inline constexpr wgsl::SourceText kTurbulenceSource{R"wgsl(struct TurbulenceParams {
  baseFreqX: f32,
  baseFreqY: f32,
  numOctaves: i32,
  seed: i32,
  stitchTiles: u32,
  typeFlag: u32,
  tileWidth: f32,
  tileHeight: f32,
  filterFromDeviceA: f32,
  filterFromDeviceB: f32,
  filterFromDeviceC: f32,
  filterFromDeviceD: f32,
}

struct TurbulenceTables {
  lattice: array<i32, 514>,
  gradX: array<f32, 2056>,
  gradY: array<f32, 2056>,
}

@group(0) @binding(0) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(1) var<storage, read> params: TurbulenceParams;
@group(0) @binding(2) var<storage, read> tables: TurbulenceTables;

fn s_curve(t: f32) -> f32 {
  return ((t * t) * (3f - (2f * t)));
}

fn lerp_f(t: f32, a: f32, b: f32) -> f32 {
  return (a + (t * (b - a)));
}

fn noise2(channel: i32, x: f32, y: f32, stitchWidth: i32, stitchHeight: i32, wrapX: i32, wrapY: i32, stitchTiles: u32) -> f32 {
  let tx = (x + 4096f);
  var bx0: i32 = i32(tx);
  var bx1: i32 = (bx0 + 1i);
  let rx0 = (tx - f32(bx0));
  let rx1 = (rx0 - 1f);
  let ty = (y + 4096f);
  var by0: i32 = i32(ty);
  var by1: i32 = (by0 + 1i);
  let ry0 = (ty - f32(by0));
  let ry1 = (ry0 - 1f);
  if ((stitchTiles == 1u)) {
    if ((bx0 >= wrapX)) {
      bx0 = (bx0 - stitchWidth);
    }
    if ((bx1 >= wrapX)) {
      bx1 = (bx1 - stitchWidth);
    }
    if ((by0 >= wrapY)) {
      by0 = (by0 - stitchHeight);
    }
    if ((by1 >= wrapY)) {
      by1 = (by1 - stitchHeight);
    }
  }
  bx0 = (((bx0 % 256i) + 256i) % 256i);
  bx1 = (((bx1 % 256i) + 256i) % 256i);
  by0 = (((by0 % 256i) + 256i) % 256i);
  by1 = (((by1 % 256i) + 256i) % 256i);
  let iValue = tables.lattice[bx0];
  let jValue = tables.lattice[bx1];
  let b00 = tables.lattice[(iValue + by0)];
  let b10 = tables.lattice[(jValue + by0)];
  let b01 = tables.lattice[(iValue + by1)];
  let b11 = tables.lattice[(jValue + by1)];
  let sx = s_curve(rx0);
  let sy = s_curve(ry0);
  let channelOffset = (channel * 514i);
  let u0 = ((tables.gradX[(channelOffset + b00)] * rx0) + (tables.gradY[(channelOffset + b00)] * ry0));
  let v0 = ((tables.gradX[(channelOffset + b10)] * rx1) + (tables.gradY[(channelOffset + b10)] * ry0));
  let aValue = lerp_f(sx, u0, v0);
  let u1 = ((tables.gradX[(channelOffset + b01)] * rx0) + (tables.gradY[(channelOffset + b01)] * ry1));
  let v1 = ((tables.gradX[(channelOffset + b11)] * rx1) + (tables.gradY[(channelOffset + b11)] * ry1));
  let bValue = lerp_f(sx, u1, v1);
  return lerp_f(sy, aValue, bValue);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coord = vec2<i32>(gid.xy);
  let px = f32(coord.x);
  let py = f32(coord.y);
  let ux = ((params.filterFromDeviceA * px) + (params.filterFromDeviceB * py));
  let uy = ((params.filterFromDeviceC * px) + (params.filterFromDeviceD * py));
  var pixel: vec4<f32> = vec4<f32>(0f);
  var frequencyX: f32 = params.baseFreqX;
  var frequencyY: f32 = params.baseFreqY;
  var inverseRatio: f32 = 1f;
  for (var octave: i32 = 0i; (octave < params.numOctaves); octave = (octave + 1i)) {
    let nx = (ux * frequencyX);
    let ny = (uy * frequencyY);
    var stitchWidth: i32 = i32((params.tileWidth * frequencyX));
    var stitchHeight: i32 = i32((params.tileHeight * frequencyY));
    if ((stitchWidth < 1i)) {
      stitchWidth = 1i;
    }
    if ((stitchHeight < 1i)) {
      stitchHeight = 1i;
    }
    let wrapX = (stitchWidth + 4096i);
    let wrapY = (stitchHeight + 4096i);
    var noise: vec4<f32> = vec4<f32>(noise2(0i, nx, ny, stitchWidth, stitchHeight, wrapX, wrapY, params.stitchTiles), noise2(1i, nx, ny, stitchWidth, stitchHeight, wrapX, wrapY, params.stitchTiles), noise2(2i, nx, ny, stitchWidth, stitchHeight, wrapX, wrapY, params.stitchTiles), noise2(3i, nx, ny, stitchWidth, stitchHeight, wrapX, wrapY, params.stitchTiles));
    if ((params.typeFlag == 1u)) {
      noise = abs(noise);
    }
    pixel = (pixel + (noise * inverseRatio));
    frequencyX = (frequencyX * 2f);
    frequencyY = (frequencyY * 2f);
    inverseRatio = (inverseRatio * 0.5f);
  }
  var rgba: vec4<f32> = vec4<f32>(0f);
  if ((params.typeFlag == 0u)) {
    rgba = clamp(((pixel + vec4<f32>(1f)) * vec4<f32>(0.5f)), vec4<f32>(0f), vec4<f32>(1f));
  } else {
    rgba = clamp(pixel, vec4<f32>(0f), vec4<f32>(1f));
  }
  textureStore(outputTexture, coord, vec4<f32>((rgba.xyz * rgba.w), rgba.w));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
