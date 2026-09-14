#pragma once
/// @file
/// Authoritative WGSL for Lambertian diffuse lighting over an alpha height map.
#include "donner/gpu/shader/wgsl/Compiler.h"
namespace donner::gpu::shader::programs {
inline constexpr wgsl::SourceText kDiffuseLightingSource{R"wgsl(struct LightingParams {
  surfaceScale: f32,
  lightingConstant: f32,
  specularExponent: f32,
  pad0: f32,
  lightR: f32,
  lightG: f32,
  lightB: f32,
  lightType: u32,
  azimuthRad: f32,
  elevationRad: f32,
  lightX: f32,
  lightY: f32,
  lightZ: f32,
  userLightX: f32,
  userLightY: f32,
  userLightZ: f32,
  pointsAtX: f32,
  pointsAtY: f32,
  pointsAtZ: f32,
  spotExponent: f32,
  userPointsAtX: f32,
  userPointsAtY: f32,
  userPointsAtZ: f32,
  coneAngleRad: f32,
  pixelToUser0: f32,
  pixelToUser1: f32,
  pixelToUser2: f32,
  pixelToUser3: f32,
  pixelToUser4: f32,
  pixelToUser5: f32,
  hasShear: u32,
  hasConeAngle: u32,
  sampleMinX: i32,
  sampleMinY: i32,
  sampleMaxX: i32,
  sampleMaxY: i32,
}

@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<storage, read> params: LightingParams;

fn safeNormalize(value: vec3<f32>) -> vec3<f32> {
  let lengthSquared = dot(value, value);
  if ((lengthSquared > 0f)) {
    return normalize(value);
  }
  return vec3<f32>(0f);
}

fn heightAt(coord: vec2<i32>) -> f32 {
  let clamped = clamp(coord, vec2<i32>(params.sampleMinX, params.sampleMinY), vec2<i32>(params.sampleMaxX, params.sampleMaxY));
  return textureLoad(inputTexture, clamped, 0i).w;
}

fn horizontalDifference(coord: vec2<i32>) -> f32 {
  if ((coord.x == params.sampleMinX)) {
    return (heightAt(vec2<i32>((coord.x + 1i), coord.y)) - heightAt(vec2<i32>(coord.x, coord.y)));
  }
  if ((coord.x == params.sampleMaxX)) {
    return (heightAt(vec2<i32>(coord.x, coord.y)) - heightAt(vec2<i32>((coord.x - 1i), coord.y)));
  }
  return (heightAt(vec2<i32>((coord.x + 1i), coord.y)) - heightAt(vec2<i32>((coord.x - 1i), coord.y)));
}

fn verticalDifference(coord: vec2<i32>) -> f32 {
  if ((coord.y == params.sampleMinY)) {
    return (heightAt(vec2<i32>(coord.x, (coord.y + 1i))) - heightAt(vec2<i32>(coord.x, coord.y)));
  }
  if ((coord.y == params.sampleMaxY)) {
    return (heightAt(vec2<i32>(coord.x, coord.y)) - heightAt(vec2<i32>(coord.x, (coord.y - 1i))));
  }
  return (heightAt(vec2<i32>(coord.x, (coord.y + 1i))) - heightAt(vec2<i32>(coord.x, (coord.y - 1i))));
}

fn computeNormal(coord: vec2<i32>) -> vec3<f32> {
  var nx: f32 = (2f * horizontalDifference(coord));
  if ((coord.y > params.sampleMinY)) {
    nx = (nx + horizontalDifference((coord + vec2<i32>(0i, -1i))));
  }
  if ((coord.y < params.sampleMaxY)) {
    nx = (nx + horizontalDifference((coord + vec2<i32>(0i, 1i))));
  }
  var ny: f32 = (2f * verticalDifference(coord));
  if ((coord.x > params.sampleMinX)) {
    ny = (ny + verticalDifference((coord + vec2<i32>(-1i, 0i))));
  }
  if ((coord.x < params.sampleMaxX)) {
    ny = (ny + verticalDifference((coord + vec2<i32>(1i, 0i))));
  }
  let divisor = select(4f, 3f, (((coord.x == params.sampleMinX) || (coord.x == params.sampleMaxX)) && ((coord.y == params.sampleMinY) || (coord.y == params.sampleMaxY))));
  return safeNormalize(vec3<f32>((((-params.surfaceScale) * nx) / divisor), (((-params.surfaceScale) * ny) / divisor), 1f));
}

fn computeLightDirection(coord: vec2<i32>, surfaceZ: f32) -> vec3<f32> {
  if ((params.lightType == 0u)) {
    return safeNormalize(vec3<f32>((cos(params.azimuthRad) * cos(params.elevationRad)), (sin(params.azimuthRad) * cos(params.elevationRad)), sin(params.elevationRad)));
  }
  return safeNormalize((vec3<f32>(params.lightX, params.lightY, params.lightZ) - vec3<f32>(f32(coord.x), f32(coord.y), surfaceZ)));
}

fn spotLightFactor(coord: vec2<i32>, alpha: f32, lightDirection: vec3<f32>) -> f32 {
  if ((params.lightType != 2u)) {
    return 1f;
  }
  let cosAngleDevice = dot((-lightDirection), safeNormalize(vec3<f32>((params.pointsAtX - params.lightX), (params.pointsAtY - params.lightY), (params.pointsAtZ - params.lightZ))));
  if ((cosAngleDevice <= 0f)) {
    return 0f;
  }
  var cosAngle: f32 = cosAngleDevice;
  if ((params.hasShear != 0u)) {
    let userX = (((params.pixelToUser0 * f32(coord.x)) + (params.pixelToUser1 * f32(coord.y))) + params.pixelToUser2);
    let userY = (((params.pixelToUser3 * f32(coord.x)) + (params.pixelToUser4 * f32(coord.y))) + params.pixelToUser5);
    cosAngle = dot(safeNormalize(vec3<f32>((userX - params.userLightX), (userY - params.userLightY), ((params.surfaceScale * alpha) - params.userLightZ))), safeNormalize(vec3<f32>((params.userPointsAtX - params.userLightX), (params.userPointsAtY - params.userLightY), (params.userPointsAtZ - params.userLightZ))));
    if ((cosAngle <= 0f)) {
      return 0f;
    }
  }
  var coneFactor: f32 = 1f;
  if ((params.hasConeAngle != 0u)) {
    let cosOuter = cos(params.coneAngleRad);
    let cosInner = (cosOuter + 0.016f);
    if ((cosAngle < cosOuter)) {
      return 0f;
    }
    if ((cosAngle < cosInner)) {
      coneFactor = ((cosAngle - cosOuter) / 0.016f);
    }
  }
  let exponent = select(1f, params.spotExponent, (params.spotExponent > 0f));
  return (pow(cosAngle, exponent) * coneFactor);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let extent = textureDimensions(outputTexture);
  if (((gid.x >= extent.x) || (gid.y >= extent.y))) {
    return;
  }
  let coord = vec2<i32>(gid.xy);
  if ((((coord.x < params.sampleMinX) || (coord.x > params.sampleMaxX)) || ((coord.y < params.sampleMinY) || (coord.y > params.sampleMaxY)))) {
    textureStore(outputTexture, coord, vec4<f32>(0f, 0f, 0f, 0f));
    return;
  }
  let normal = computeNormal(coord);
  let alpha = heightAt(coord);
  let surfaceZ = (params.surfaceScale * alpha);
  let lightDirection = computeLightDirection(coord, surfaceZ);
  let spot = spotLightFactor(coord, alpha, lightDirection);
  let intensity = ((params.lightingConstant * max(dot(normal, lightDirection), 0f)) * spot);
  let red = clamp((intensity * params.lightR), 0f, 1f);
  let green = clamp((intensity * params.lightG), 0f, 1f);
  let blue = clamp((intensity * params.lightB), 0f, 1f);
  textureStore(outputTexture, coord, vec4<f32>(red, green, blue, 1f));
}
)wgsl"};
}  // namespace donner::gpu::shader::programs
