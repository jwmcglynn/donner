#include "donner/svg/renderer/geode/GeodePipeline.h"

#include "donner/svg/renderer/geode/GeodeShaders.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

GeodePipeline::GeodePipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // ----- Bind group layout -----
  // Seven bindings: uniforms, bands SSBO, curves SSBO, pattern texture,
  // pattern sampler, clip-mask texture, clip-mask sampler. The pattern
  // texture/sampler are only sampled when paintMode == "pattern" and
  // the clip-mask texture/sampler only when `hasClipMask != 0`. A 1x1
  // dummy texture is bound for both when the feature is inactive so
  // the bind group layout is stable across draw calls.
  wgpu::BindGroupLayoutEntry entries[7] = {};

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

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = wgpuLabel("GeodeSlugFillBGL");
  bglDesc.entryCount = 7;
  bglDesc.entries = entries;
  bindGroupLayout_ = device.createBindGroupLayout(bglDesc);

  // ----- Pipeline layout -----
  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = wgpuLabel("GeodeSlugFillPL");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout_};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

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
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  // ----- Render pipeline -----
  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = wgpuLabel("GeodeSlugFill");
  rpDesc.layout = pipelineLayout;

  rpDesc.vertex.module = shader;
  rpDesc.vertex.entryPoint = wgpuLabel("vs_main");
  rpDesc.vertex.bufferCount = 1;
  rpDesc.vertex.buffers = &vbLayout;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  // 4× MSAA. Slug's per-pixel winding test is binary — without multisample
  // coverage, thin axis-aligned strokes stair-step at pixel boundaries and
  // diverge from tiny-skia's 4× analytic AA (preserveAspectRatio cluster).
  // The fragment shader writes a `@builtin(sample_mask)` computed from 4
  // sub-pixel winding tests; the hardware resolve step averages the
  // surviving samples into the 1-sample resolve attachment.
  rpDesc.multisample.count = 4;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_ = device.createRenderPipeline(rpDesc);
}

// ============================================================================
// GeodeGradientPipeline
// ============================================================================

GeodeGradientPipeline::GeodeGradientPipeline(const wgpu::Device& device,
                                             wgpu::TextureFormat colorFormat)
    : colorFormat_(colorFormat) {
  // Five bindings — uniforms, bands SSBO, curves SSBO, clip-mask texture,
  // clip-mask sampler. The clip-mask bindings always carry something
  // valid; when `hasClipMask == 0` a 1x1 dummy texture is bound and
  // the shader skips the sample work.
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

  // Phase 3b clip mask texture + sampler.
  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Fragment;
  entries[3].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[3].texture.multisampled = false;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = wgpuLabel("GeodeSlugGradientBGL");
  bglDesc.entryCount = 5;
  bglDesc.entries = entries;
  bindGroupLayout_ = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = wgpuLabel("GeodeSlugGradientPL");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout_};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

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
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = wgpuLabel("GeodeSlugGradient");
  rpDesc.layout = pipelineLayout;

  rpDesc.vertex.module = shader;
  rpDesc.vertex.entryPoint = wgpuLabel("vs_main");
  rpDesc.vertex.bufferCount = 1;
  rpDesc.vertex.buffers = &vbLayout;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  // 4× MSAA to match the solid-fill pipeline — see its multisample comment.
  rpDesc.multisample.count = 4;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_ = device.createRenderPipeline(rpDesc);
}

// ============================================================================
// GeodeMaskPipeline
// ============================================================================

GeodeMaskPipeline::GeodeMaskPipeline(const wgpu::Device& device) {
  // Five bindings — uniforms, bands SSBO, curves SSBO, nested clip
  // mask texture, nested clip mask sampler. The clip-mask slot is
  // always bound; a 1x1 dummy is used when `uniforms.hasClipMask ==
  // 0` so the draw stays valid without layout variants.
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
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[3].texture.multisampled = false;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].sampler.type = wgpu::SamplerBindingType::Filtering;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = wgpuLabel("GeodeSlugMaskBGL");
  bglDesc.entryCount = 5;
  bglDesc.entries = entries;
  bindGroupLayout_ = device.createBindGroupLayout(bglDesc);

  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = wgpuLabel("GeodeSlugMaskPL");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout_};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

  wgpu::ShaderModule shader = createSlugMaskShader(device);

  // Same vertex buffer layout as the fill pipelines: pos (vec2f) +
  // normal (vec2f) + bandIndex (u32) = 20 bytes per vertex.
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

  // Max-blend so multiple clip paths rendered into the same mask layer
  // UNION. Each shader writes `1.0` for covered samples, `0.0` would
  // otherwise be the clear value, so `Max` over the red channel keeps
  // the larger coverage — exactly the union.
  wgpu::BlendState blend = {};
  blend.color.srcFactor = wgpu::BlendFactor::One;
  blend.color.dstFactor = wgpu::BlendFactor::One;
  blend.color.operation = wgpu::BlendOperation::Max;
  blend.alpha.srcFactor = wgpu::BlendFactor::One;
  blend.alpha.dstFactor = wgpu::BlendFactor::One;
  blend.alpha.operation = wgpu::BlendOperation::Max;

  wgpu::ColorTargetState colorTarget = {};
  colorTarget.format = wgpu::TextureFormat::R8Unorm;
  colorTarget.blend = &blend;
  colorTarget.writeMask = wgpu::ColorWriteMask::Red;

  wgpu::FragmentState fragmentState = {};
  fragmentState.module = shader;
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = wgpuLabel("GeodeSlugMask");
  rpDesc.layout = pipelineLayout;

  rpDesc.vertex.module = shader;
  rpDesc.vertex.entryPoint = wgpuLabel("vs_main");
  rpDesc.vertex.bufferCount = 1;
  rpDesc.vertex.buffers = &vbLayout;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  // 4× MSAA matching the colour pipelines. The mask texture is
  // allocated MSAA-4 with a 1-sample R8 resolve target; the main fill
  // pipelines sample the resolved texture to pick up fractional
  // coverage at the clip edge.
  rpDesc.multisample.count = 4;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_ = device.createRenderPipeline(rpDesc);
}

}  // namespace donner::geode
