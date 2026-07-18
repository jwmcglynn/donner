#include "donner/svg/renderer/geode/GeodePipeline.h"

#include "donner/svg/renderer/geode/GeodeShaders.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

GeodePipeline::GeodePipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // ----- Bind group layout -----
  // Fourteen bindings: uniforms, bands SSBO, curves SSBO, pattern texture,
  // pattern sampler, clip-mask texture, clip-mask sampler, and
  // (Milestone 6 Bullet 2) per-instance transforms SSBO. The pattern
  // texture/sampler are only sampled when paintMode == "pattern" and
  // the clip-mask texture/sampler only when `hasClipMask != 0`. A 1x1
  // dummy texture is bound for both when the feature is inactive so
  // the bind group layout is stable across draw calls. The
  // instance-transforms buffer is always bound too - a 1-element
  // identity buffer (`GeodeDevice::identityInstanceTransformBuffer`)
  // for single-draw fills, a full per-instance array for
  // `fillPathInstanced`.
  wgpu::BindGroupLayoutEntry entries[14] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[0].buffer.minBindingSize = 0;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Fragment;
  entries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[1].buffer.minBindingSize = 0;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Fragment;
  entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[2].buffer.minBindingSize = 0;

  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Fragment;
  entries[3].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[3].texture.multisampled = false;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

  // Phase 3b clip mask texture + sampler.
  entries[5].binding = 5;
  entries[5].visibility = wgpu::ShaderStage::Fragment;
  entries[5].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[5].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[5].texture.multisampled = false;

  entries[6].binding = 6;
  entries[6].visibility = wgpu::ShaderStage::Fragment;
  entries[6].sampler.type = wgpu::SamplerBindingType::Filtering;

  // M6 Bullet 2: per-instance affine transforms (vertex-visible only).
  entries[7].binding = 7;
  entries[7].visibility = wgpu::ShaderStage::Vertex;
  entries[7].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[7].buffer.minBindingSize = 0;

  // Analytic dual-ray fill (0041 §8): vertical bands SSBO, vertical curves
  // SSBO, horizontal band grid, vertical band grid, and compact references
  // into each canonical curve array. All fragment-read-only.
  entries[8].binding = 8;
  entries[8].visibility = wgpu::ShaderStage::Fragment;
  entries[8].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[8].buffer.minBindingSize = 0;

  entries[9].binding = 9;
  entries[9].visibility = wgpu::ShaderStage::Fragment;
  entries[9].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[9].buffer.minBindingSize = 0;

  entries[10].binding = 10;
  entries[10].visibility = wgpu::ShaderStage::Fragment;
  entries[10].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[10].buffer.minBindingSize = 0;

  entries[11].binding = 11;
  entries[11].visibility = wgpu::ShaderStage::Fragment;
  entries[11].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[11].buffer.minBindingSize = 0;

  for (int i = 12; i <= 13; ++i) {
    entries[i].binding = static_cast<uint32_t>(i);
    entries[i].visibility = wgpu::ShaderStage::Fragment;
    entries[i].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[i].buffer.minBindingSize = 0;
  }

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = wgpuLabel("GeodeSlugFillBGL");
  bglDesc.entryCount = 14;
  bglDesc.entries = entries;
  bindGroupLayout_.reset(device.createBindGroupLayout(bglDesc));

  // ----- Pipeline layout -----
  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = wgpuLabel("GeodeSlugFillPL");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout_.get()};
  plDesc.bindGroupLayouts = layouts;
  ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(device.createPipelineLayout(plDesc));

  // ----- Shader module -----
  ScopedWgpuHandle<wgpu::ShaderModule> shader(createSlugFillShader(device));

  // ----- Fragment / blending -----
  wgpu::BlendState blend = {};
  // Standard premultiplied-alpha source-over blending.
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
  fragmentState.module = shader.get();
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  // ----- Render pipeline -----
  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = wgpuLabel("GeodeSlugFill");
  rpDesc.layout = pipelineLayout.get();

  rpDesc.vertex.module = shader.get();
  rpDesc.vertex.entryPoint = wgpuLabel("vs_main");
  rpDesc.vertex.bufferCount = 0;
  rpDesc.vertex.buffers = nullptr;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  rpDesc.multisample.count = 1;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_.reset(device.createRenderPipeline(rpDesc));
}

// ============================================================================
// GeodeGradientPipeline
// ============================================================================

GeodeGradientPipeline::GeodeGradientPipeline(const wgpu::Device& device,
                                             wgpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // Eleven bindings - uniforms, H bands SSBO, H curves SSBO, clip-mask texture,
  // clip-mask sampler, and (analytic dual-ray, 0041 §8) V bands SSBO, V curves
  // SSBO, H band grid, V band grid. The clip-mask bindings always carry
  // something valid; when `hasClipMask == 0` a 1x1 dummy texture is bound and
  // the shader skips the sample work.
  wgpu::BindGroupLayoutEntry entries[11] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[0].buffer.minBindingSize = 0;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Fragment;
  entries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[1].buffer.minBindingSize = 0;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Fragment;
  entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[2].buffer.minBindingSize = 0;

  // Phase 3b clip mask texture + sampler.
  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Fragment;
  entries[3].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[3].texture.multisampled = false;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

  // Analytic dual-ray (0041 §8): vertical bands/curves + dense band grids.
  for (int i = 5; i <= 10; ++i) {
    entries[i].binding = static_cast<uint32_t>(i);
    entries[i].visibility = wgpu::ShaderStage::Fragment;
    entries[i].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[i].buffer.minBindingSize = 0;
  }

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = wgpuLabel("GeodeSlugGradientBGL");
  bglDesc.entryCount = 11;
  bglDesc.entries = entries;
  bindGroupLayout_.reset(device.createBindGroupLayout(bglDesc));

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = wgpuLabel("GeodeSlugGradientPL");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout_.get()};
  plDesc.bindGroupLayouts = layouts;
  ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(device.createPipelineLayout(plDesc));

  ScopedWgpuHandle<wgpu::ShaderModule> shader(createSlugGradientShader(device));

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
  fragmentState.module = shader.get();
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = wgpuLabel("GeodeSlugGradient");
  rpDesc.layout = pipelineLayout.get();

  rpDesc.vertex.module = shader.get();
  rpDesc.vertex.entryPoint = wgpuLabel("vs_main");
  rpDesc.vertex.bufferCount = 0;
  rpDesc.vertex.buffers = nullptr;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  rpDesc.multisample.count = 1;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_.reset(device.createRenderPipeline(rpDesc));
}

// ============================================================================
// GeodeMaskPipeline
// ============================================================================

GeodeMaskPipeline::GeodeMaskPipeline(const wgpu::Device& device) {
  // Eleven bindings - uniforms, H bands SSBO, H curves SSBO, nested clip mask
  // texture, nested clip mask sampler, and (analytic dual-ray, 0041 §8) V bands
  // SSBO, V curves SSBO, H band grid, V band grid. The clip-mask slot is always
  // bound; a 1x1 dummy is used when `uniforms.hasClipMask == 0`.
  wgpu::BindGroupLayoutEntry entries[11] = {};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[0].buffer.minBindingSize = 0;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Fragment;
  entries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[1].buffer.minBindingSize = 0;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Fragment;
  entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  entries[2].buffer.minBindingSize = 0;

  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Fragment;
  entries[3].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[3].texture.multisampled = false;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

  // Analytic dual-ray (0041 §8): vertical bands/curves + dense band grids.
  for (int i = 5; i <= 10; ++i) {
    entries[i].binding = static_cast<uint32_t>(i);
    entries[i].visibility = wgpu::ShaderStage::Fragment;
    entries[i].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[i].buffer.minBindingSize = 0;
  }

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = wgpuLabel("GeodeSlugMaskBGL");
  bglDesc.entryCount = 11;
  bglDesc.entries = entries;
  bindGroupLayout_.reset(device.createBindGroupLayout(bglDesc));

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = wgpuLabel("GeodeSlugMaskPL");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout_.get()};
  plDesc.bindGroupLayouts = layouts;
  ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(device.createPipelineLayout(plDesc));

  ScopedWgpuHandle<wgpu::ShaderModule> shader(createSlugMaskShader(device));

  // Max-blend unions scalar analytic coverage from multiple clip paths.
  wgpu::BlendState blend = {};
  blend.color.srcFactor = wgpu::BlendFactor::One;
  blend.color.dstFactor = wgpu::BlendFactor::One;
  blend.color.operation = wgpu::BlendOperation::Max;
  blend.alpha.srcFactor = wgpu::BlendFactor::One;
  blend.alpha.dstFactor = wgpu::BlendFactor::One;
  blend.alpha.operation = wgpu::BlendOperation::Max;

  wgpu::ColorTargetState colorTarget = {};
  colorTarget.format = wgpu::TextureFormat::RGBA8Unorm;
  colorTarget.blend = &blend;
  colorTarget.writeMask = wgpu::ColorWriteMask::All;

  wgpu::FragmentState fragmentState = {};
  fragmentState.module = shader.get();
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = wgpuLabel("GeodeSlugMask");
  rpDesc.layout = pipelineLayout.get();

  rpDesc.vertex.module = shader.get();
  rpDesc.vertex.entryPoint = wgpuLabel("vs_main");
  rpDesc.vertex.bufferCount = 0;
  rpDesc.vertex.buffers = nullptr;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  rpDesc.multisample.count = 1;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_.reset(device.createRenderPipeline(rpDesc));
}

}  // namespace donner::geode
