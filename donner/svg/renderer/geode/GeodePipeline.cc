#include "donner/svg/renderer/geode/GeodePipeline.h"

#include "donner/svg/renderer/geode/GeodeShaders.h"

namespace donner::geode {

GeodePipeline::GeodePipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // ----- Bind group layout -----
  // Three bindings: uniforms, bands SSBO, curves SSBO.
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
  bglDesc.label = "GeodeSlugFillBGL";
  bglDesc.entryCount = 3;
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

}  // namespace donner::geode
