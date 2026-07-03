#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"

#include <string_view>

#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

namespace {

constexpr std::string_view kCheckerboardWgsl = R"wgsl(
struct Params {
  target_size: vec2<f32>,
  device_pixel_ratio: f32,
  checker_size: f32,
  dark_color: vec4<f32>,
  light_color: vec4<f32>,
};

@group(0) @binding(0) var<uniform> params: Params;

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> @builtin(position) vec4<f32> {
  let positions = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0),
    vec2<f32>(3.0, -1.0),
    vec2<f32>(-1.0, 3.0)
  );
  return vec4<f32>(positions[vertex_index], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
  let screen = min(position.xy, params.target_size) / max(params.device_pixel_ratio, 0.0001);
  let cell = vec2<i32>(floor(screen / vec2<f32>(params.checker_size, params.checker_size)));
  if ((cell.x + cell.y) % 2 == 0) {
    return params.light_color;
  }

  return params.dark_color;
}
)wgsl";

}  // namespace

GeodeCheckerboardPipeline::GeodeCheckerboardPipeline(const wgpu::Device& device,
                                                     wgpu::TextureFormat colorFormat) {
  wgpu::BindGroupLayoutEntry layoutEntry = {};
  layoutEntry.binding = 0;
  layoutEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
  layoutEntry.buffer.type = wgpu::BufferBindingType::Uniform;
  layoutEntry.buffer.minBindingSize = sizeof(Uniforms);

  wgpu::BindGroupLayoutDescriptor layoutDesc = {};
  layoutDesc.label = wgpuLabel("GeodeCheckerboardBGL");
  layoutDesc.entryCount = 1;
  layoutDesc.entries = &layoutEntry;
  bindGroupLayout_ = device.createBindGroupLayout(layoutDesc);
  if (!bindGroupLayout_) {
    return;
  }

  wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
  wgslSource.code.data = kCheckerboardWgsl.data();
  wgslSource.code.length = kCheckerboardWgsl.size();

  wgpu::ShaderModuleDescriptor shaderDesc{wgpu::Default};
  shaderDesc.label = wgpuLabel("GeodeCheckerboard");
  shaderDesc.nextInChain = &wgslSource.chain;
  ScopedWgpuHandle<wgpu::ShaderModule> shader(device.createShaderModule(shaderDesc));
  if (!shader) {
    return;
  }

  wgpu::PipelineLayoutDescriptor pipelineLayoutDesc = {};
  pipelineLayoutDesc.label = wgpuLabel("GeodeCheckerboardPL");
  pipelineLayoutDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout bindGroupLayouts[1] = {bindGroupLayout_};
  pipelineLayoutDesc.bindGroupLayouts = bindGroupLayouts;
  ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(
      device.createPipelineLayout(pipelineLayoutDesc));
  if (!pipelineLayout) {
    return;
  }

  wgpu::ColorTargetState colorTarget = {};
  colorTarget.format = colorFormat;
  colorTarget.writeMask = wgpu::ColorWriteMask::All;

  wgpu::FragmentState fragmentState = {};
  fragmentState.module = shader.get();
  fragmentState.entryPoint = wgpuLabel("fs_main");
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  wgpu::RenderPipelineDescriptor pipelineDesc = {};
  pipelineDesc.label = wgpuLabel("GeodeCheckerboard");
  pipelineDesc.layout = pipelineLayout.get();
  pipelineDesc.vertex.module = shader.get();
  pipelineDesc.vertex.entryPoint = wgpuLabel("vs_main");
  pipelineDesc.vertex.bufferCount = 0;
  pipelineDesc.vertex.buffers = nullptr;
  pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
  pipelineDesc.fragment = &fragmentState;
  pipelineDesc.multisample.count = 1;
  pipelineDesc.multisample.mask = 0xFFFFFFFF;
  pipeline_ = device.createRenderPipeline(pipelineDesc);
}

}  // namespace donner::geode
