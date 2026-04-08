#pragma once
/// @file
/// WGSL shader sources used by the Slug rendering pipeline.

#include <string_view>
#include <webgpu/webgpu_cpp.h>

namespace donner::geode {

/**
 * Hard-coded WGSL source for the Slug fill pipeline.
 *
 * Contains both the vertex shader (`vs_main`) and fragment shader (`fs_main`).
 * The shader expects:
 *
 * - `@group(0) @binding(0) var<uniform> uniforms: Uniforms;`
 * - `@group(0) @binding(1) var<storage, read> bands: array<Band>;`
 * - `@group(0) @binding(2) var<storage, read> curveData: array<f32>;`
 *
 * and vertex attributes:
 * - `@location(0) pos: vec2f`      — path-space position
 * - `@location(1) normal: vec2f`   — outward normal for dilation
 * - `@location(2) bandIndex: u32`  — which band this vertex belongs to
 */
extern const std::string_view kSlugFillShaderWgsl;

/**
 * Compile the Slug fill shader for the given device.
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createSlugFillShader(const wgpu::Device& device);

}  // namespace donner::geode
