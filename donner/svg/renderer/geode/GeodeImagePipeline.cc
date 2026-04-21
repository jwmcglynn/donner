#include "donner/svg/renderer/geode/GeodeImagePipeline.h"

#include "donner/svg/renderer/geode/GeodeShaders.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

GeodeImagePipeline::GeodeImagePipeline(const wgpu::Device& device, wgpu::TextureFormat colorFormat,
                                       uint32_t sampleCount)
    : colorFormat_(colorFormat) {
  // ----- Bind group layout -----
  // Seven bindings: uniform buffer, sampler, sampled content texture,
  // sampled luminance-mask texture (Phase 3c), sampled parent-
  // snapshot texture (Phase 3d blend modes), sampled Phase 3b
  // clip-mask texture, clip-mask sampler. Optional textures always
  // bind some valid view so the layout stays stable across every draw.
  wgpu::BindGroupLayoutEntry entries[7] = {};

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
  entries[2].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[2].texture.multisampled = false;

  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Fragment;
  entries[3].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[3].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[3].texture.multisampled = false;

  entries[4].binding = 4;
  entries[4].visibility = wgpu::ShaderStage::Fragment;
  entries[4].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[4].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[4].texture.multisampled = false;

  entries[5].binding = 5;
  entries[5].visibility = wgpu::ShaderStage::Fragment;
  entries[5].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[5].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[5].texture.multisampled = false;

  entries[6].binding = 6;
  entries[6].visibility = wgpu::ShaderStage::Fragment;
  entries[6].sampler.type = wgpu::SamplerBindingType::Filtering;

  wgpu::BindGroupLayoutDescriptor bglDesc = {};
  bglDesc.label = wgpuLabel("GeodeImageBlitBGL");
  bglDesc.entryCount = 7;
  bglDesc.entries = entries;
  bindGroupLayout_ = device.createBindGroupLayout(bglDesc);

  // ----- Pipeline layout -----
  wgpu::PipelineLayoutDescriptor plDesc = {};
  plDesc.label = wgpuLabel("GeodeImageBlitPL");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bindGroupLayout_};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = device.createPipelineLayout(plDesc);

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
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  // ----- Render pipeline -----
  // No vertex buffers — the shader generates corners from vertex_index.
  wgpu::RenderPipelineDescriptor rpDesc = {};
  rpDesc.label = wgpuLabel("GeodeImageBlit");
  rpDesc.layout = pipelineLayout;
  rpDesc.vertex.module = shader;
  rpDesc.vertex.entryPoint = wgpuLabel("vs_main");
  rpDesc.vertex.bufferCount = 0;
  rpDesc.vertex.buffers = nullptr;

  rpDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  rpDesc.primitive.cullMode = wgpu::CullMode::None;

  rpDesc.fragment = &fragmentState;
  // Match the Slug fill + gradient pipelines' sample count. When the
  // alpha-coverage path is active this is 1 (no MSAA).
  rpDesc.multisample.count = sampleCount;
  rpDesc.multisample.mask = 0xFFFFFFFF;

  pipeline_ = device.createRenderPipeline(rpDesc);

  // ----- Samplers -----
  // Linear (bilinear) sampler — the default for SVG's "smooth" image
  // rendering.
  //
  // `{wgpu::Default}` runs `SamplerDescriptor::setDefault()` (clamp-to-edge,
  // nearest filtering, lodMaxClamp=32) and critically `maxAnisotropy = 1`,
  // which wgpu-native validates as non-zero. Plain `= {}` would leave
  // `maxAnisotropy = 0` and fail createSampler validation on native.
  wgpu::SamplerDescriptor linearDesc{wgpu::Default};
  linearDesc.label = wgpuLabel("GeodeImageBlitLinear");
  linearDesc.magFilter = wgpu::FilterMode::Linear;
  linearDesc.minFilter = wgpu::FilterMode::Linear;
  linearDesc.maxAnisotropy = 1;
  linearSampler_ = device.createSampler(linearDesc);

  // Nearest sampler — for `image-rendering: pixelated`.
  wgpu::SamplerDescriptor nearestDesc{wgpu::Default};
  nearestDesc.label = wgpuLabel("GeodeImageBlitNearest");
  nearestDesc.maxAnisotropy = 1;
  nearestSampler_ = device.createSampler(nearestDesc);

  wgpu::SamplerDescriptor clipMaskDesc{wgpu::Default};
  clipMaskDesc.label = wgpuLabel("GeodeImageBlitClipMask");
  clipMaskDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
  clipMaskDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
  clipMaskDesc.magFilter = wgpu::FilterMode::Linear;
  clipMaskDesc.minFilter = wgpu::FilterMode::Linear;
  clipMaskDesc.maxAnisotropy = 1;
  clipMaskSampler_ = device.createSampler(clipMaskDesc);
}

}  // namespace donner::geode
