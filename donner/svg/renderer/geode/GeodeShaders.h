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

/**
 * Compile the Gaussian blur compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/gaussian_blur.wgsl` via the `embed_resources()` Bazel rule.
 * The shader implements a two-pass separable Gaussian blur with
 * configurable edge-mode handling (None / Duplicate / Wrap).
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var input_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(2) var<uniform> params: BlurParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createGaussianBlurShader(const wgpu::Device& device);

/**
 * Compile the feOffset compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_offset.wgsl` via the `embed_resources()` Bazel rule.
 * The shader shifts input pixels by a uniform (dx, dy) offset with
 * configurable edge-mode handling.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var input_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(2) var<uniform> params: OffsetParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterOffsetShader(const wgpu::Device& device);

/**
 * Compile the feColorMatrix compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_color_matrix.wgsl` via the `embed_resources()` Bazel rule.
 * The shader applies a 4x5 color matrix to each pixel's RGBA channels.
 * All type variants (matrix, saturate, hueRotate, luminanceToAlpha) are
 * pre-computed to a 4x5 matrix on the CPU side.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var input_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(2) var<uniform> params: ColorMatrixParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterColorMatrixShader(const wgpu::Device& device);

/**
 * Compile the feFlood compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_flood.wgsl` via the `embed_resources()` Bazel rule.
 * The shader fills every pixel with a constant color uniform.
 * No input texture is required.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(1) var<uniform> params: FloodParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterFloodShader(const wgpu::Device& device);

/**
 * Compile the feMerge alpha-over blit compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_merge.wgsl` via the `embed_resources()` Bazel rule.
 * The shader composites a source texture over a destination texture using
 * Porter-Duff source-over: `out = src + dst * (1 - src.a)`.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var src_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var dst_tex: texture_2d<f32>;`
 * - `@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterMergeShader(const wgpu::Device& device);

/**
 * Compile the feComposite compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_composite.wgsl` via the `embed_resources()` Bazel rule.
 * The shader applies one of 7 Porter-Duff compositing operators to two
 * premultiplied-alpha input textures.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var in1_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var in2_tex: texture_2d<f32>;`
 * - `@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(3) var<uniform> params: CompositeParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterCompositeShader(const wgpu::Device& device);

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
 * Compile the feMorphology compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_morphology.wgsl` via the `embed_resources()` Bazel rule.
 * The shader applies a rectangular min/max kernel for erode/dilate.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var input_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(2) var<uniform> params: MorphologyParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterMorphologyShader(const wgpu::Device& device);

/**
 * Compile the feComponentTransfer compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_component_transfer.wgsl` via the `embed_resources()` Bazel rule.
 * The shader un-premultiplies, applies per-channel 256-entry LUTs, then
 * re-premultiplies.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var input_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(2) var<storage, read> params: ComponentTransferParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterComponentTransferShader(const wgpu::Device& device);

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

/**
 * Compile the feTurbulence compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_turbulence.wgsl` via the `embed_resources()` Bazel rule.
 * The shader generates Perlin noise / fractal noise patterns per the SVG
 * feTurbulence specification.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(1) var<storage, read> params: TurbulenceParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterTurbulenceShader(const wgpu::Device& device);

/**
 * Compile the feDisplacementMap compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_displacement_map.wgsl` via the `embed_resources()` Bazel rule.
 * The shader displaces each output pixel from in1 by channel values read from in2.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var in1_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var in2_tex: texture_2d<f32>;`
 * - `@group(0) @binding(2) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(3) var<uniform> params: DisplacementParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterDisplacementMapShader(const wgpu::Device& device);

/**
 * Compile the feDiffuseLighting compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_diffuse_lighting.wgsl` via the `embed_resources()` Bazel rule.
 * The shader computes Lambertian diffuse lighting from the input's alpha channel
 * used as a height map. Supports distant, point, and spot light sources via uniforms.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var input_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(2) var<storage, read> params: LightingParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterDiffuseLightingShader(const wgpu::Device& device);

/**
 * Compile the feSpecularLighting compute shader for the given device.
 *
 * The WGSL source is embedded at build time from
 * `shaders/filter_specular_lighting.wgsl` via the `embed_resources()` Bazel rule.
 * The shader computes Phong specular lighting from the input's alpha channel
 * used as a height map. Supports distant, point, and spot light sources via uniforms.
 *
 * Bind group layout:
 * - `@group(0) @binding(0) var input_tex: texture_2d<f32>;`
 * - `@group(0) @binding(1) var output_tex: texture_storage_2d<rgba8unorm, write>;`
 * - `@group(0) @binding(2) var<storage, read> params: LightingParams;`
 *
 * @return A valid shader module on success, or an empty module if compilation
 *   failed (errors go to the device's uncaptured error callback).
 */
wgpu::ShaderModule createFilterSpecularLightingShader(const wgpu::Device& device);

}  // namespace donner::geode
