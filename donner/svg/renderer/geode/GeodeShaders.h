#pragma once
/// @file
/// Frozen WGSL modules used by the live Slug rendering pipelines.
#include "donner/gpu/Device.h"
namespace donner::geode {
/// Creates the SlugFill module from its frozen WGSL artifact.
/// Ordinary and batched fills share reflected draw and instance records.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @return Shader module or creation error.
gpu::Result<gpu::ShaderModule> createSlugFillShader(gpu::Device& device);

/// Creates the SlugGradient module from its frozen WGSL artifact.
/// Linear and radial gradients share the verified gradient parameter block.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @return Shader module or creation error.
gpu::Result<gpu::ShaderModule> createSlugGradientShader(gpu::Device& device);

/// Creates the SlugMask module from its frozen WGSL artifact.
/// The fragment writes scalar clip coverage to an RGBA8Unorm target.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @return Shader module or creation error.
gpu::Result<gpu::ShaderModule> createSlugMaskShader(gpu::Device& device);

/// Creates the ImageBlit module from its frozen WGSL artifact.
/// The quad is generated from the vertex index and needs no vertex buffers.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @return Shader module or creation error.
gpu::Result<gpu::ShaderModule> createImageBlitShader(gpu::Device& device);

}  // namespace donner::geode
