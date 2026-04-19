#include "donner/svg/renderer/geode/GeodeFilterEngine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <variant>

#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeShaders.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

namespace {

constexpr wgpu::TextureFormat kFormat = wgpu::TextureFormat::RGBA8Unorm;

/// Uniform buffer layout matching the WGSL `BlurParams` struct.
struct BlurParams {
  float stdDeviation;
  uint32_t axis;      // 0 = horizontal, 1 = vertical.
  uint32_t edgeMode;  // 0 = None, 1 = Duplicate, 2 = Wrap.
  uint32_t pad;
};

/// Map a FilterGraph EdgeMode to the shader's uint.
uint32_t toShaderEdgeMode(svg::components::filter_primitive::GaussianBlur::EdgeMode mode) {
  using EM = svg::components::filter_primitive::GaussianBlur::EdgeMode;
  switch (mode) {
    case EM::None: return 0;
    case EM::Duplicate: return 1;
    case EM::Wrap: return 2;
  }
  return 0;
}

/// Create a texture usable as both a compute output (storage) and a
/// subsequent compute / render input (texture binding).
wgpu::Texture createIntermediateTexture(const wgpu::Device& device, uint32_t width, uint32_t height,
                                        const char* label) {
  wgpu::TextureDescriptor td{};
  td.label = wgpuLabel(label);
  td.size = {width, height, 1};
  td.format = kFormat;
  td.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding |
             wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  return device.createTexture(td);
}

}  // namespace

GeodeFilterEngine::GeodeFilterEngine(GeodeDevice& device, bool verbose)
    : device_(device), verbose_(verbose) {
  const wgpu::Device& dev = device_.device();

  // --- Bind group layout ---
  // Binding 0: input texture (sampled)
  // Binding 1: output texture (write-only storage)
  // Binding 2: uniform buffer (BlurParams)
  wgpu::BindGroupLayoutEntry entries[3]{};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[0].texture.multisampled = false;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  entries[1].storageTexture.format = kFormat;
  entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Compute;
  entries[2].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[2].buffer.minBindingSize = sizeof(BlurParams);

  wgpu::BindGroupLayoutDescriptor bglDesc{};
  bglDesc.label = wgpuLabel("GaussianBlurBGL");
  bglDesc.entryCount = 3;
  bglDesc.entries = entries;
  blurBindGroupLayout_ = dev.createBindGroupLayout(bglDesc);

  // --- Compute pipeline ---
  wgpu::ShaderModule shaderModule = createGaussianBlurShader(dev);

  wgpu::PipelineLayoutDescriptor plDesc{};
  plDesc.label = wgpuLabel("GaussianBlurPipelineLayout");
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {blurBindGroupLayout_};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

  wgpu::ComputePipelineDescriptor cpDesc{};
  cpDesc.label = wgpuLabel("GaussianBlurPipeline");
  cpDesc.layout = pipelineLayout;
  cpDesc.compute.module = shaderModule;
  cpDesc.compute.entryPoint = wgpuLabel("main");
  gaussianBlurPipeline_ = dev.createComputePipeline(cpDesc);
}

GeodeFilterEngine::~GeodeFilterEngine() = default;

wgpu::Texture GeodeFilterEngine::execute(const svg::components::FilterGraph& graph,
                                         const wgpu::Texture& sourceGraphic,
                                         const Box2d& filterRegion) {
  using namespace svg::components;

  // Named intermediate textures keyed by the `result` attribute.
  std::unordered_map<std::string, wgpu::Texture> namedBuffers;

  // The "previous" buffer — output of the last node (or sourceGraphic for
  // the first node).
  wgpu::Texture currentBuffer = sourceGraphic;

  for (const FilterNode& node : graph.nodes) {
    // Resolve the input texture for this node. Multi-input primitives are
    // not yet supported — take the first input or fall back to the
    // previous buffer.
    wgpu::Texture inputTex = currentBuffer;
    if (!node.inputs.empty()) {
      const FilterInput& firstInput = node.inputs[0];
      if (const auto* named = std::get_if<FilterInput::Named>(&firstInput.value)) {
        auto it = namedBuffers.find(named->name.str());
        if (it != namedBuffers.end()) {
          inputTex = it->second;
        }
      } else if (std::holds_alternative<FilterInput::Previous>(firstInput.value)) {
        inputTex = currentBuffer;
      }
      // StandardInput::SourceGraphic → sourceGraphic
      else if (const auto* stdIn = std::get_if<FilterStandardInput>(&firstInput.value)) {
        if (*stdIn == FilterStandardInput::SourceGraphic) {
          inputTex = sourceGraphic;
        } else if (*stdIn == FilterStandardInput::SourceAlpha) {
          // SourceAlpha is not yet implemented — use SourceGraphic as
          // a rough approximation (the alpha channel is there, just
          // premultiplied with color).
          inputTex = sourceGraphic;
        } else {
          // Other standard inputs (FillPaint, StrokePaint, etc.) are
          // not yet wired — fall through to sourceGraphic.
          inputTex = sourceGraphic;
        }
      }
    }

    // Dispatch based on primitive type.
    wgpu::Texture outputTex;
    if (const auto* blur = std::get_if<filter_primitive::GaussianBlur>(&node.primitive)) {
      const double sx = std::max(blur->stdDeviationX, 0.0);
      const double sy = std::max(blur->stdDeviationY, 0.0);
      const uint32_t em = toShaderEdgeMode(blur->edgeMode);
      outputTex = applyGaussianBlur(inputTex, sx, sy, em);
    } else {
      // Unsupported primitive — pass through unchanged.
      if (verbose_ && !warnedUnsupported_) {
        std::cerr << "GeodeFilterEngine: unsupported filter primitive "
                     "(passthrough)\n";
        warnedUnsupported_ = true;
      }
      outputTex = inputTex;
    }

    // Store named result if the node has one.
    if (node.result.has_value()) {
      namedBuffers[node.result->str()] = outputTex;
    }

    currentBuffer = outputTex;
  }

  return currentBuffer;
}

wgpu::Texture GeodeFilterEngine::applyGaussianBlur(const wgpu::Texture& input, double stdDeviationX,
                                                   double stdDeviationY, uint32_t edgeMode) {
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  // stdDeviation == 0 on both axes → no-op passthrough.
  if (stdDeviationX <= 0.0 && stdDeviationY <= 0.0) {
    return input;
  }

  // Pass 1: horizontal blur (axis = 0).
  wgpu::Texture afterHorizontal = input;
  if (stdDeviationX > 0.0) {
    afterHorizontal =
        runBlurPass(input, width, height, static_cast<float>(stdDeviationX), /*axis=*/0, edgeMode);
  }

  // Pass 2: vertical blur (axis = 1).
  wgpu::Texture afterVertical = afterHorizontal;
  if (stdDeviationY > 0.0) {
    afterVertical = runBlurPass(afterHorizontal, width, height, static_cast<float>(stdDeviationY),
                                /*axis=*/1, edgeMode);
  }

  return afterVertical;
}

wgpu::Texture GeodeFilterEngine::runBlurPass(const wgpu::Texture& input, uint32_t width,
                                             uint32_t height, float stdDeviation, uint32_t axis,
                                             uint32_t edgeMode) {
  const wgpu::Device& dev = device_.device();

  // Output texture for this pass.
  wgpu::Texture output = createIntermediateTexture(dev, width, height, "GaussianBlurPass");

  // Upload uniform buffer.
  BlurParams params{};
  params.stdDeviation = stdDeviation;
  params.axis = axis;
  params.edgeMode = edgeMode;
  params.pad = 0;

  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("BlurParamsUniform");
  bufDesc.size = sizeof(BlurParams);
  bufDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer uniformBuffer = dev.createBuffer(bufDesc);
  device_.queue().writeBuffer(uniformBuffer, 0, &params, sizeof(params));

  // Build the bind group.
  wgpu::TextureView inputView = input.createView();
  wgpu::TextureView outputView = output.createView();

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = inputView;
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = outputView;
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = uniformBuffer;
  bgEntries[2].offset = 0;
  bgEntries[2].size = sizeof(BlurParams);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("GaussianBlurBindGroup");
  bgDesc.layout = blurBindGroupLayout_;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  // Encode and submit the compute pass.
  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel("GaussianBlurEncoder");
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor cpDesc{};
  cpDesc.label = wgpuLabel("GaussianBlurPass");
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(cpDesc);
  pass.setPipeline(gaussianBlurPipeline_);
  pass.setBindGroup(0, bindGroup, 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.end();

  wgpu::CommandBuffer cmdBuf = encoder.finish();
  device_.queue().submit(1, &cmdBuf);

  return output;
}

}  // namespace donner::geode
