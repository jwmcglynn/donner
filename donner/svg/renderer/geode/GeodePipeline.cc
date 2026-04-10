#include "donner/svg/renderer/geode/GeodePipeline.h"

#include "donner/svg/renderer/geode/GeodeShaders.h"

namespace donner::geode {

GeodePipeline::GeodePipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // ----- Bind group layout -----
  // Five bindings: uniforms, bands SSBO, curves SSBO, pattern texture,
  // pattern sampler. The texture + sampler are used only when the paintMode
  // uniform is set to "pattern"; in solid-fill mode a 1x1 dummy texture is
  // bound and the shader never samples it. A single layout keeps the
  // pipeline stable across both fill modes.
  wgpu::BindGroupLayoutEntry entries[5] = {};

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
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::e2D;
  entries[3].texture.multisampled = false;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = "GeodeSlugFillBGL";
  bglDesc.entryCount = 5;
  bglDesc.entries = entries;
  bindGroupLayout_ = device.CreateBindGroupLayout(&bglDesc);

  // ----- Pipeline layout -----
  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = "GeodeSlugFillPL";
  plDesc.bindGroupLayoutCount = 1;
  wgpu::BindGroupLayout layouts[1] = {bindGroupLayout_};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&plDesc);

  // ----- Shader module -----
  wgpu::ShaderModule shader = createSlugFillShader(device);

  // ----- Vertex buffer layout -----
  // Matches EncodedPath::Vertex: pos (vec2f) + normal (vec2f) + bandIndex (u32)
  // = 5 × 4 bytes = 20 bytes per vertex.
  wgpu::VertexAttribute vertexAttribs[3] = {};
  vertexAttribs[0].format = wgpu::VertexFormat::Float32x2;
  vertexAttribs[0].offset = 0;
  vertexAttribs[0].shaderLocation = 0;

  vertexAttribs[1].format = wgpu::VertexFormat::Float32x2;
  vertexAttribs[1].offset = 8;
  vertexAttribs[1].shaderLocation = 1;

  vertexAttribs[2].format = wgpu::VertexFormat::Uint32;
  vertexAttribs[2].offset = 16;
  vertexAttribs[2].shaderLocation = 2;

  wgpu::VertexBufferLayout vbLayout = {};
  vbLayout.arrayStride = 20;
  vbLayout.stepMode = wgpu::VertexStepMode::Vertex;
  vbLayout.attributeCount = 3;
  vbLayout.attributes = vertexAttribs;

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
  fragmentState.module = shader;
  fragmentState.entryPoint = "fs_main";
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  // ----- Render pipeline -----
  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = "GeodeSlugFill";
  rpDesc.layout = pipelineLayout;

  rpDesc.vertex.module = shader;
  rpDesc.vertex.entryPoint = "vs_main";
  rpDesc.vertex.bufferCount = 1;
  rpDesc.vertex.buffers = &vbLayout;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  rpDesc.multisample.count = 1;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_ = device.CreateRenderPipeline(&rpDesc);
}

// ============================================================================
// GeodeGradientPipeline
// ============================================================================

GeodeGradientPipeline::GeodeGradientPipeline(const wgpu::Device& device,
                                             wgpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // Three bindings — same shape as the solid-fill pipeline, but the uniform
  // buffer is larger (it carries gradient parameters).
  wgpu::BindGroupLayoutEntry entries[3] = {};

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

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = "GeodeSlugGradientBGL";
  bglDesc.entryCount = 3;
  bglDesc.entries = entries;
  bindGroupLayout_ = device.CreateBindGroupLayout(&bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = "GeodeSlugGradientPL";
  plDesc.bindGroupLayoutCount = 1;
  wgpu::BindGroupLayout layouts[1] = {bindGroupLayout_};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.CreatePipelineLayout(&plDesc);

  wgpu::ShaderModule shader = createSlugGradientShader(device);

  // Same vertex buffer layout as the solid-fill pipeline.
  wgpu::VertexAttribute vertexAttribs[3] = {};
  vertexAttribs[0].format = wgpu::VertexFormat::Float32x2;
  vertexAttribs[0].offset = 0;
  vertexAttribs[0].shaderLocation = 0;

  vertexAttribs[1].format = wgpu::VertexFormat::Float32x2;
  vertexAttribs[1].offset = 8;
  vertexAttribs[1].shaderLocation = 1;

  vertexAttribs[2].format = wgpu::VertexFormat::Uint32;
  vertexAttribs[2].offset = 16;
  vertexAttribs[2].shaderLocation = 2;

  wgpu::VertexBufferLayout vbLayout = {};
  vbLayout.arrayStride = 20;
  vbLayout.stepMode = wgpu::VertexStepMode::Vertex;
  vbLayout.attributeCount = 3;
  vbLayout.attributes = vertexAttribs;

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

  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = "GeodeSlugGradient";
  rpDesc.layout = pipelineLayout;

  rpDesc.vertex.module = shader;
  rpDesc.vertex.entryPoint = "vs_main";
  rpDesc.vertex.bufferCount = 1;
  rpDesc.vertex.buffers = &vbLayout;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  rpDesc.multisample.count = 1;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_ = device.CreateRenderPipeline(&rpDesc);
}

}  // namespace donner::geode
