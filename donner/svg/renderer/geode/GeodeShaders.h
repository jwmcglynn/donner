#pragma once
/// @file
/// WGSL shader sources used by the Slug rendering pipeline.

#include <webgpu/webgpu.hpp>

namespace donner::geode {

/**
 * Compile the Slug fill shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/slug_fill.wgsl` via the `embed_resources()` Bazel rule. The shader
 * expects:
 *
 * - `@group(0) @binding(0) var<uniform> uniforms: Uniforms;`
 * - `@group(0) @binding(1) var<storage, read> bands: array<Band>;`
 * - `@group(0) @binding(2) var<storage, read> curveData: array<f32>;`
 * - `@group(0) @binding(3) var patternTexture: texture_2d<f32>;`
 * - `@group(0) @binding(4) var patternSampler: sampler;`
 *
 * and vertex attributes:
 * - `@location(0) pos: vec2f`      — path-space position
 * - `@location(1) normal: vec2f`   — outward normal for dilation
 * - `@location(2) bandIndex: u32`  — which band this vertex belongs to
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createSlugFillShader(const wgpu::Device& device);

/**
 * Compile the Slug gradient-fill shader for the given device.
 *
 * Parallel to `createSlugFillShader`, but bound to a different uniform
 * layout that carries linear-gradient parameters (transform, start/end,
 * spread mode, stops) alongside the Slug coverage machinery. See
 * `shaders/slug_gradient.wgsl` for the exact struct layout.
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createSlugGradientShader(const wgpu::Device& device);

/**
 * Compile the path-clip mask shader for the given device.
 *
 * Same band/curve encoding as @ref createSlugFillShader but the fragment
 * stage writes a single-channel coverage value into an R8Unorm target.
 * The uniform layout is reduced to just the mvp matrix, viewport size,
 * and fill rule — no paint mode, no pattern, no clip polygon. Used by
 * the Phase 3b path-clipping pipeline to materialise a per-pixel clip
 * mask texture that subsequent fill / gradient draws sample as a
 * coverage multiplier.
 */
wgpu::ShaderModule createSlugMaskShader(const wgpu::Device& device);

/**
 * Compile the alpha-coverage variant of the Slug fill shader.
 *
 * Identical to `createSlugFillShader` except the fragment stage has no
 * `@builtin(sample_mask)` output — coverage is folded into the fragment
 * color as `popcount(mask) / 4.0`. Used on Intel + Vulkan where writing
 * `sample_mask` from overlapping band quads hangs Mesa ANV / Xe KMD.
 */
wgpu::ShaderModule createSlugFillAlphaCoverageShader(const wgpu::Device& device);

/// Alpha-coverage variant of `createSlugGradientShader`.
wgpu::ShaderModule createSlugGradientAlphaCoverageShader(const wgpu::Device& device);

/// Alpha-coverage variant of `createSlugMaskShader`.
wgpu::ShaderModule createSlugMaskAlphaCoverageShader(const wgpu::Device& device);

/**
 * Compile the image-blit shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/image_blit.wgsl` via the `embed_resources()` Bazel rule. The
 * shader expects:
 *
 * - `@group(0) @binding(0) var<uniform> uniforms: Uniforms;`
 * - `@group(0) @binding(1) var imageSampler: sampler;`
 * - `@group(0) @binding(2) var imageTexture: texture_2d<f32>;`
 *
 * and no vertex buffer — corners are generated from `@builtin(vertex_index)`.
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createImageBlitShader(const wgpu::Device& device);

}  // namespace donner::geode
