#pragma once
/// @file
/// Frozen shader modules used by the live Slug rendering pipelines. Each creator selects the
/// projection the device consumes: the platform-native artifact for a Metal or Vulkan device,
/// and the authored WGSL artifact everywhere else.
#include "donner/gpu/Device.h"
namespace donner::geode {
/// Creates the SlugFill module from the frozen artifact matching the device's source kind.
/// Ordinary and batched fills share reflected draw and instance records.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @return Shader module or creation error.
gpu::Result<gpu::ShaderModule> createSlugFillShader(gpu::Device& device);

/// Creates the SlugGradient module from the frozen artifact matching the device's source kind.
/// Linear and radial gradients share the verified gradient parameter block.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @return Shader module or creation error.
gpu::Result<gpu::ShaderModule> createSlugGradientShader(gpu::Device& device);

/// Creates the SlugMask module from the frozen artifact matching the device's source kind.
/// The fragment writes scalar clip coverage to an RGBA8Unorm target.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @return Shader module or creation error.
gpu::Result<gpu::ShaderModule> createSlugMaskShader(gpu::Device& device);

/// Creates the ImageBlit module from the frozen artifact matching the device's source kind.
/// The quad is generated from the vertex index and needs no vertex buffers.
/// @param device GPU device receiving the prevalidated source and metadata.
/// @return Shader module or creation error.
gpu::Result<gpu::ShaderModule> createImageBlitShader(gpu::Device& device);

}  // namespace donner::geode
