#pragma once
/// @file
/// WGSL shader sources used by the Slug rendering pipeline.

#include <webgpu/webgpu.hpp>

#include "donner/gpu/Device.h"

namespace donner::geode {

// The four pipeline-family creators below build shader modules through the donner::gpu runtime;
// the filter creators further down still compile through wgpu directly, because the runtime
// does not model the compute passes they feed.

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
 * - `@location(0) pos: vec2f`      - path-space position
 * - `@location(1) normal: vec2f`   - outward normal for dilation
 * - `@location(2) bandIndex: u32`  - which band this vertex belongs to
 *
 * @param device Donner GPU device (in this transition phase, the wgpu adapter).
 * @return The shader module handle, or the creation error.
 */
gpu::Result<gpu::ShaderModule> createSlugFillShader(gpu::Device& device);

/**
 * Compile the Slug gradient-fill shader for the given device.
 *
 * Parallel to `createSlugFillShader`, but bound to a different uniform
 * layout that carries linear-gradient parameters (transform, start/end,
 * spread mode, stops) alongside the Slug coverage machinery. See
 * `shaders/slug_gradient.wgsl` for the exact struct layout.
 *
 * @param device Donner GPU device (in this transition phase, the wgpu adapter).
 * @return The shader module handle, or the creation error.
 */
gpu::Result<gpu::ShaderModule> createSlugGradientShader(gpu::Device& device);

/**
 * Compile the path-clip mask shader for the given device.
 *
 * Same band/curve encoding as @ref createSlugFillShader but the fragment
 * stage writes clip coverage into an RGBA8Unorm target.
 * The uniform layout is reduced to just the mvp matrix, viewport size,
 * and fill rule - no paint mode, no pattern, no clip polygon. Used by
 * the path-clipping pipeline to materialise a per-pixel clip
 * mask texture that subsequent fill / gradient draws sample as a
 * coverage multiplier.
 *
 * @param device Donner GPU device (in this transition phase, the wgpu adapter).
 * @return The shader module handle, or the creation error.
 */
gpu::Result<gpu::ShaderModule> createSlugMaskShader(gpu::Device& device);

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
 * and no vertex buffer - corners are generated from `@builtin(vertex_index)`.
 *
 * @param device Donner GPU device (in this transition phase, the wgpu adapter).
 * @return The shader module handle, or the creation error.
 */
gpu::Result<gpu::ShaderModule> createImageBlitShader(gpu::Device& device);

/**
 * Compile the feBlend compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_blend.wgsl` via the `embed_resources()` Bazel rule.
 * The shader applies one of 16 W3C Compositing 1 blend modes to two
 * premultiplied-alpha input textures, using the same blend formulas as
 * `image_blit.wgsl`.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var in1_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var in2_tex: texture_2d<f32>;`
 * - `@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(3) var<uniform> params: BlendParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterBlendShader(const wgpu::Device& device);

/**
 * Compile the feConvolveMatrix compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_convolve_matrix.wgsl` via the `embed_resources()` Bazel rule.
 * The shader applies an NxM kernel convolution with configurable edge mode,
 * divisor, bias, and preserveAlpha.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var input_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(2) var<uniform> params: ConvolveParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterConvolveMatrixShader(const wgpu::Device& device);

}  // namespace donner::geode
