#include "donner/svg/renderer/geode/GeodeImagePipeline.h"

#include "donner/svg/renderer/geode/GeodeShaders.h"

namespace donner::geode {

GeodeImagePipeline::GeodeImagePipeline(const wgpu::Device& device,
                                       wgpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // ----- Bind group layout -----
  // Five bindings: uniform buffer, sampler, sampled content texture,
  // sampled luminance-mask texture (Phase 3c), sampled parent-
  // snapshot texture (Phase 3d blend modes). The mask and snapshot
  // textures are only read when their respective uniform flags are
  // non-zero; in normal blit mode both bind to a 1x1 dummy so the
  // bind group layout stays stable across every draw.
  wgpu::BindGroupLayoutEntry entries[5] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[0].buffer.minBindingSize = 0;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Fragment;
  entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Fragment;
  entries[2].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;
  entries[2].texture.multisampled = false;

  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Fragment;
  entries[3].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;
  entries[3].texture.multisampled = false;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[4].texture.viewDimension = wgpu::TextureViewDimension::e2D;
  entries[4].texture.multisampled = false;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = "GeodeImageBlitBGL";
  bglDesc.entryCount = 5;
  bglDesc.entries = entries;
  bindGroupLayout_ = device.CreateBindGroupLayout(&bglDesc);

  // ----- Pipeline layout -----
  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = "GeodeImageBlitPL";
  plDesc.bindGroupLayoutCount = 1;
  wgpu::BindGroupLayout layouts[1] = {bindGroupLayout_};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&plDesc);

  // ----- Shader module -----
  wgpu::ShaderModule shader = createImageBlitShader(device);

  // ----- Fragment / blending -----
  // Same premultiplied-source-over as the Slug fill pipeline. The fragment
  // shader premultiplies the straight-alpha texture sample before emitting
  // the color so this blend equation is correct.
  wgpu::BlendState blend = {};
  blend.color.srcFactor = wgpu::BlendFactor::One;
  blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blend.color.operation = wgpu::BlendOperation::Add;
  blend.alpha.srcFactor = wgpu::BlendFactor::One;
  blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blend.alpha.operation = wgpu::BlendOperation::Add;

  wgpu::ColorTargetState colorTarget = {};
  colorTarget.format = colorFormat_;
  colorTarget.blend = &blend;
  colorTarget.writeMask = wgpu::ColorWriteMask::All;

  wgpu::FragmentState fragmentState = {};
  fragmentState.module = shader;
  fragmentState.entryPoint = "fs_main";
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  // ----- Render pipeline -----
  // No vertex buffers — the shader generates corners from vertex_index.
  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = "GeodeImageBlit";
  rpDesc.layout = pipelineLayout;
  rpDesc.vertex.module = shader;
  rpDesc.vertex.entryPoint = "vs_main";
  rpDesc.vertex.bufferCount = 0;
  rpDesc.vertex.buffers = nullptr;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  // 4× MSAA to match the Slug fill + gradient pipelines. WebGPU requires
  // all pipelines used against the same render pass attachment to agree
  // on sample count, and the Geode encoder always targets MSAA textures.
  // Image blit itself doesn't need per-sample coverage (the quad is
  // fully-covering), but the pipeline must still advertise 4×.
  rpDesc.multisample.count = 4;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_ = device.CreateRenderPipeline(&rpDesc);

  // ----- Samplers -----
  // Linear (bilinear) sampler — the default for SVG's "smooth" image
  // rendering.
  wgpu::SamplerDescriptor linearDesc = {};
  linearDesc.label = "GeodeImageBlitLinear";
  linearDesc.magFilter = wgpu::FilterMode::Linear;
  linearDesc.minFilter = wgpu::FilterMode::Linear;
  linearDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
  linearDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
  linearDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
  linearDesc.addressModeW = wgpu::AddressMode::ClampToEdge;
  linearSampler_ = device.CreateSampler(&linearDesc);

  // Nearest sampler — for `image-rendering: pixelated`.
  wgpu::SamplerDescriptor nearestDesc = {};
  nearestDesc.label = "GeodeImageBlitNearest";
  nearestDesc.magFilter = wgpu::FilterMode::Nearest;
  nearestDesc.minFilter = wgpu::FilterMode::Nearest;
  nearestDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
  nearestDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
  nearestDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
  nearestDesc.addressModeW = wgpu::AddressMode::ClampToEdge;
  nearestSampler_ = device.CreateSampler(&nearestDesc);
}

}  // namespace donner::geode
