#pragma once
/// @file
/// Focused WGSL fixture for the numeric and control-flow features used by the Slug mask.

#include "donner/gpu/shader/wgsl/Compiler.h"

namespace donner::gpu::shader::wgsl::tests {

inline constexpr SourceText kControlSource{R"wgsl(
const kNoBand: u32 = 0xFFFFFFFFu;
struct Item { value: f32, };
@group(0) @binding(3) var<storage,read> values: array<Item>;
@group(0) @binding(4) var unusedSampler: sampler;
fn accumulate(index: u32) -> f32 {
  var total=0.0;
  for(var i=0u; i < 4u; i=i+1u) {
    if (i == index) { continue; }
    for(var j=0u; j < 3u; j=j+1u) {
      if (j == index) { break; }
      if ((j & 1u) == 0u) { total=total+values[j].value; }
    }
    if (index == kNoBand) { break; } else if (index == 4u) { total=0.7071068; }
  }
  return total;
}
@fragment fn fs_main(@builtin(position) p: vec4f) -> @location(0) vec4f {
  let width=fwidth(p.x);
  if (p.x < 0.0) { discard; }
  let v=vec2f(p.xy);
  let scale=1.0 / 65536.0;
  let bounded=max(abs(p.y),1e-6);
  let large=3.402823466e38;
  let root=sqrt(bounded);
  let d=dot(normalize(v),v);
  let out=round(root)+length(v)+fract(p.x)+width;
  return vec4f(saturate(accumulate(u32(p.x))+out+d*scale));
}
)wgsl"};

/// Uniform switch selection across implicit sampling, explicit LOD, and integer-coordinate loads.
inline constexpr SourceText kSamplingSwitchSource{R"wgsl(
struct Params { mode: u32, level: f32, padding: vec2f, }
@group(0) @binding(0) var image: texture_2d<f32>;
@group(0) @binding(1) var imageSampler: sampler;
@group(0) @binding(2) var<uniform> params: Params;
@fragment fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let uv = position.xy * 0.01f;
  var color = vec4f(0f);
  switch (params.mode) {
    case 0u: { color = textureSample(image, imageSampler, uv); }
    case 1u: { color = textureSampleLevel(image, imageSampler, uv, params.level); }
    default: { color = textureLoad(image, vec2i(position.xy), 0i); }
  }
  return color;
}
)wgsl"};

}  // namespace donner::gpu::shader::wgsl::tests
