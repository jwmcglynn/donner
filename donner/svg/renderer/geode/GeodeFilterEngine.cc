#include "donner/svg/renderer/geode/GeodeFilterEngine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
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

/// Uniform buffer layout matching the WGSL `OffsetParams` struct.
struct OffsetParams {
  float dx;
  float dy;
  uint32_t edgeMode;
  uint32_t pad;
};

/// Uniform buffer layout matching the WGSL `ColorMatrixParams` struct.
/// 4x5 matrix stored as 5 column vectors (each vec4f = one column across
/// R'/G'/B'/A' rows).
struct ColorMatrixParams {
  float col0[4];  // multipliers for R input
  float col1[4];  // multipliers for G input
  float col2[4];  // multipliers for B input
  float col3[4];  // multipliers for A input
  float col4[4];  // constant offset
};

/// Uniform buffer layout matching the WGSL `FloodParams` struct.
struct FloodParams {
  float color[4];  // RGBA flood color in straight alpha.
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

/// Helper to create a pipeline with a standard (input, output, uniform) bind group layout.
/// Used by blur, offset, and color-matrix pipelines.
struct InputOutputUniformPipeline {
  wgpu::BindGroupLayout bindGroupLayout;
  wgpu::ComputePipeline pipeline;
};

InputOutputUniformPipeline createInputOutputUniformPipeline(const wgpu::Device& dev,
                                                           const char* label,
                                                           wgpu::ShaderModule shaderModule,
                                                           size_t uniformSize) {
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
  entries[2].buffer.minBindingSize = uniformSize;

  std::string bglLabel = std::string(label) + "BGL";
  wgpu::BindGroupLayoutDescriptor bglDesc{};
  bglDesc.label = wgpuLabel(bglLabel.c_str());
  bglDesc.entryCount = 3;
  bglDesc.entries = entries;
  auto bgl = dev.createBindGroupLayout(bglDesc);

  std::string plLabel = std::string(label) + "PipelineLayout";
  wgpu::PipelineLayoutDescriptor plDesc{};
  plDesc.label = wgpuLabel(plLabel.c_str());
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bgl};
  plDesc.bindGroupLayouts = layouts;
  wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

  std::string cpLabel = std::string(label) + "Pipeline";
  wgpu::ComputePipelineDescriptor cpDesc{};
  cpDesc.label = wgpuLabel(cpLabel.c_str());
  cpDesc.layout = pipelineLayout;
  cpDesc.compute.module = shaderModule;
  cpDesc.compute.entryPoint = wgpuLabel("main");
  auto pipeline = dev.createComputePipeline(cpDesc);

  return {bgl, pipeline};
}

/// Dispatch a compute shader with a standard (input, output, uniform) bind group.
void dispatchInputOutputUniform(GeodeDevice& device, const wgpu::BindGroupLayout& bgl,
                                const wgpu::ComputePipeline& pipeline,
                                const wgpu::Texture& input, const wgpu::Texture& output,
                                const wgpu::Buffer& uniformBuffer, size_t uniformSize,
                                const char* label) {
  const wgpu::Device& dev = device.device();
  const uint32_t width = output.getWidth();
  const uint32_t height = output.getHeight();

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
  bgEntries[2].size = uniformSize;

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel(label);
  bgDesc.layout = bgl;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel(label);
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel(label);
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(pipeline);
  pass.setBindGroup(0, bindGroup, 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.end();

  wgpu::CommandBuffer cmdBuf = encoder.finish();
  device.queue().submit(1, &cmdBuf);
}

/// Create a uniform buffer and upload data to it.
wgpu::Buffer createUniformBuffer(GeodeDevice& device, const void* data, size_t size,
                                 const char* label) {
  const wgpu::Device& dev = device.device();
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel(label);
  bufDesc.size = size;
  bufDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer buffer = dev.createBuffer(bufDesc);
  device.queue().writeBuffer(buffer, 0, data, size);
  return buffer;
}

/// Resolve an input reference to a texture.
wgpu::Texture resolveInput(const svg::components::FilterInput& input,
                           const std::unordered_map<std::string, wgpu::Texture>& namedBuffers,
                           const wgpu::Texture& currentBuffer,
                           const wgpu::Texture& sourceGraphic) {
  using namespace svg::components;
  if (const auto* named = std::get_if<FilterInput::Named>(&input.value)) {
    auto it = namedBuffers.find(named->name.str());
    if (it != namedBuffers.end()) {
      return it->second;
    }
  } else if (std::holds_alternative<FilterInput::Previous>(input.value)) {
    return currentBuffer;
  } else if (const auto* stdIn = std::get_if<FilterStandardInput>(&input.value)) {
    if (*stdIn == FilterStandardInput::SourceGraphic ||
        *stdIn == FilterStandardInput::SourceAlpha) {
      return sourceGraphic;
    }
    return sourceGraphic;
  }
  return currentBuffer;
}

/// Build the 4x5 color matrix from feColorMatrix parameters.
/// All type variants are pre-computed here so the shader only needs a
/// single generic matrix multiply.
ColorMatrixParams buildColorMatrix(
    const svg::components::filter_primitive::ColorMatrix& primitive) {
  ColorMatrixParams params{};

  auto setIdentity = [&]() {
    // Identity: R'=R, G'=G, B'=B, A'=A, offsets=0.
    params.col0[0] = 1.0f;  // R' from R
    params.col1[1] = 1.0f;  // G' from G
    params.col2[2] = 1.0f;  // B' from B
    params.col3[3] = 1.0f;  // A' from A
  };

  using Type = svg::components::filter_primitive::ColorMatrix::Type;

  switch (primitive.type) {
    case Type::Matrix: {
      if (primitive.values.size() >= 20) {
        // Row-major 4x5 → column-major 5×4.
        for (int row = 0; row < 4; ++row) {
          params.col0[row] = static_cast<float>(primitive.values[row * 5 + 0]);
          params.col1[row] = static_cast<float>(primitive.values[row * 5 + 1]);
          params.col2[row] = static_cast<float>(primitive.values[row * 5 + 2]);
          params.col3[row] = static_cast<float>(primitive.values[row * 5 + 3]);
          params.col4[row] = static_cast<float>(primitive.values[row * 5 + 4]);
        }
      } else {
        setIdentity();
      }
      break;
    }

    case Type::Saturate: {
      const float s = primitive.values.empty() ? 1.0f : static_cast<float>(primitive.values[0]);
      // SVG spec saturate matrix:
      //   | 0.213+0.787s  0.715-0.715s  0.072-0.072s  0  0 |
      //   | 0.213-0.213s  0.715+0.285s  0.072-0.072s  0  0 |
      //   | 0.213-0.213s  0.715-0.715s  0.072+0.928s  0  0 |
      //   | 0             0             0              1  0 |
      params.col0[0] = 0.213f + 0.787f * s;
      params.col0[1] = 0.213f - 0.213f * s;
      params.col0[2] = 0.213f - 0.213f * s;
      params.col0[3] = 0.0f;
      params.col1[0] = 0.715f - 0.715f * s;
      params.col1[1] = 0.715f + 0.285f * s;
      params.col1[2] = 0.715f - 0.715f * s;
      params.col1[3] = 0.0f;
      params.col2[0] = 0.072f - 0.072f * s;
      params.col2[1] = 0.072f - 0.072f * s;
      params.col2[2] = 0.072f + 0.928f * s;
      params.col2[3] = 0.0f;
      params.col3[3] = 1.0f;
      break;
    }

    case Type::HueRotate: {
      const double angleDeg =
          primitive.values.empty() ? 0.0 : primitive.values[0];
      const double rad = angleDeg * std::numbers::pi / 180.0;
      const float c = static_cast<float>(std::cos(rad));
      const float s = static_cast<float>(std::sin(rad));
      // SVG spec hueRotate matrix (from the spec table).
      params.col0[0] = 0.213f + 0.787f * c - 0.213f * s;
      params.col0[1] = 0.213f - 0.213f * c + 0.143f * s;
      params.col0[2] = 0.213f - 0.213f * c - 0.787f * s;
      params.col0[3] = 0.0f;
      params.col1[0] = 0.715f - 0.715f * c - 0.715f * s;
      params.col1[1] = 0.715f + 0.285f * c + 0.140f * s;
      params.col1[2] = 0.715f - 0.715f * c + 0.715f * s;
      params.col1[3] = 0.0f;
      params.col2[0] = 0.072f - 0.072f * c + 0.928f * s;
      params.col2[1] = 0.072f - 0.072f * c - 0.283f * s;
      params.col2[2] = 0.072f + 0.928f * c + 0.072f * s;
      params.col2[3] = 0.0f;
      params.col3[3] = 1.0f;
      break;
    }

    case Type::LuminanceToAlpha: {
      // R'=0, G'=0, B'=0, A'= 0.2126*R + 0.7152*G + 0.0722*B.
      params.col0[3] = 0.2126f;
      params.col1[3] = 0.7152f;
      params.col2[3] = 0.0722f;
      break;
    }
  }

  return params;
}

}  // namespace

GeodeFilterEngine::GeodeFilterEngine(GeodeDevice& device, bool verbose)
    : device_(device), verbose_(verbose) {
  const wgpu::Device& dev = device_.device();

  // --- Gaussian blur pipeline (existing) ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "GaussianBlur", createGaussianBlurShader(dev), sizeof(BlurParams));
    blurBindGroupLayout_ = bgl;
    gaussianBlurPipeline_ = pipeline;
  }

  // --- feOffset pipeline ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "FilterOffset", createFilterOffsetShader(dev), sizeof(OffsetParams));
    offsetBindGroupLayout_ = bgl;
    offsetPipeline_ = pipeline;
  }

  // --- feColorMatrix pipeline ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "FilterColorMatrix", createFilterColorMatrixShader(dev), sizeof(ColorMatrixParams));
    colorMatrixBindGroupLayout_ = bgl;
    colorMatrixPipeline_ = pipeline;
  }

  // --- feFlood pipeline (output + uniform, no input) ---
  {
    wgpu::BindGroupLayoutEntry entries[2]{};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
    entries[0].storageTexture.format = kFormat;
    entries[0].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[1].buffer.minBindingSize = sizeof(FloodParams);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterFloodBGL");
    bglDesc.entryCount = 2;
    bglDesc.entries = entries;
    floodBindGroupLayout_ = dev.createBindGroupLayout(bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterFloodPipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {floodBindGroupLayout_};
    plDesc.bindGroupLayouts = layouts;
    wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterFloodPipeline");
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = createFilterFloodShader(dev);
    cpDesc.compute.entryPoint = wgpuLabel("main");
    floodPipeline_ = dev.createComputePipeline(cpDesc);
  }

  // --- feMerge alpha-over pipeline (src, dst → output) ---
  {
    wgpu::BindGroupLayoutEntry entries[3]{};
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
    entries[0].texture.multisampled = false;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[1].texture.viewDimension = wgpu::TextureViewDimension::_2D;
    entries[1].texture.multisampled = false;

    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Compute;
    entries[2].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
    entries[2].storageTexture.format = kFormat;
    entries[2].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterMergeBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    mergeBindGroupLayout_ = dev.createBindGroupLayout(bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterMergePipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {mergeBindGroupLayout_};
    plDesc.bindGroupLayouts = layouts;
    wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterMergePipeline");
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = createFilterMergeShader(dev);
    cpDesc.compute.entryPoint = wgpuLabel("main");
    mergePipeline_ = dev.createComputePipeline(cpDesc);
  }
}

GeodeFilterEngine::~GeodeFilterEngine() = default;

wgpu::Texture GeodeFilterEngine::execute(const svg::components::FilterGraph& graph,
                                         const wgpu::Texture& sourceGraphic,
                                         const Box2d& filterRegion) {
  using namespace svg::components;

  std::unordered_map<std::string, wgpu::Texture> namedBuffers;
  wgpu::Texture currentBuffer = sourceGraphic;

  // Compute user-space → pixel-space scale factors so that primitive
  // parameters (dx/dy, stdDeviation, etc.) are interpreted in pixels.
  const double scaleX = graph.userToPixelScale.x > 0.0 ? graph.userToPixelScale.x : 1.0;
  const double scaleY = graph.userToPixelScale.y > 0.0 ? graph.userToPixelScale.y : 1.0;

  // For objectBoundingBox primitiveUnits, values are relative to the element
  // bounding box.  Scale through the bbox dimensions first, then to pixels.
  const bool isOBB =
      graph.primitiveUnits == svg::PrimitiveUnits::ObjectBoundingBox;
  const double bboxW =
      graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->width() : 1.0;
  const double bboxH =
      graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->height() : 1.0;
  auto toPixelX = [&](double val) -> double {
    return std::abs(isOBB ? val * bboxW : val) * scaleX;
  };
  auto toPixelY = [&](double val) -> double {
    return std::abs(isOBB ? val * bboxH : val) * scaleY;
  };

  for (const FilterNode& node : graph.nodes) {
    // Resolve the primary input texture for this node.
    wgpu::Texture inputTex = currentBuffer;
    if (!node.inputs.empty()) {
      inputTex = resolveInput(node.inputs[0], namedBuffers, currentBuffer, sourceGraphic);
    }

    // Dispatch based on primitive type.
    wgpu::Texture outputTex;
    if (const auto* blur = std::get_if<filter_primitive::GaussianBlur>(&node.primitive)) {
      const double sx = blur->stdDeviationX >= 0 ? toPixelX(blur->stdDeviationX) : 0.0;
      const double sy = blur->stdDeviationY >= 0 ? toPixelY(blur->stdDeviationY) : 0.0;
      const uint32_t em = toShaderEdgeMode(blur->edgeMode);
      outputTex = applyGaussianBlur(inputTex, sx, sy, em);
    } else if (const auto* offset = std::get_if<filter_primitive::Offset>(&node.primitive)) {
      // Scale offset from user-space to pixel-space.  Unlike blur, offset
      // preserves sign (negative shifts are valid), so do not take abs().
      filter_primitive::Offset scaled = *offset;
      if (isOBB) {
        scaled.dx *= bboxW;
        scaled.dy *= bboxH;
      }
      scaled.dx *= scaleX;
      scaled.dy *= scaleY;
      outputTex = applyOffset(inputTex, scaled);
    } else if (const auto* cm = std::get_if<filter_primitive::ColorMatrix>(&node.primitive)) {
      outputTex = applyColorMatrix(inputTex, *cm);
    } else if (const auto* flood = std::get_if<filter_primitive::Flood>(&node.primitive)) {
      outputTex = applyFlood(inputTex.getWidth(), inputTex.getHeight(), *flood);
    } else if (std::holds_alternative<filter_primitive::Merge>(node.primitive)) {
      outputTex = applyMerge(node, namedBuffers, currentBuffer, sourceGraphic);
    } else {
      // Unsupported primitive — pass through unchanged.
      if (verbose_ && !warnedUnsupported_) {
        std::cerr << "GeodeFilterEngine: unsupported filter primitive "
                     "(passthrough)\n";
        warnedUnsupported_ = true;
      }
      outputTex = inputTex;
    }

    if (node.result.has_value()) {
      namedBuffers[node.result->str()] = outputTex;
    }

    currentBuffer = outputTex;
  }

  // Clip the final output to the filter region.  The SVG filter model
  // requires pixels outside the filter region to be transparent.  We
  // clear a fresh texture via a render pass and then GPU-copy only the
  // pixels inside the region from the result.
  //
  // The filter region is in user-space coordinates; transform to pixel
  // coordinates using the graph's scale factor.
  {
    const uint32_t width = currentBuffer.getWidth();
    const uint32_t height = currentBuffer.getHeight();
    const double scaleX = graph.userToPixelScale.x > 0.0 ? graph.userToPixelScale.x : 1.0;
    const double scaleY = graph.userToPixelScale.y > 0.0 ? graph.userToPixelScale.y : 1.0;
    const int x0 =
        std::max(0, static_cast<int>(std::floor(filterRegion.topLeft.x * scaleX)));
    const int y0 =
        std::max(0, static_cast<int>(std::floor(filterRegion.topLeft.y * scaleY)));
    const int x1 =
        std::clamp(static_cast<int>(std::ceil(filterRegion.bottomRight.x * scaleX)), 0,
                   static_cast<int>(width));
    const int y1 =
        std::clamp(static_cast<int>(std::ceil(filterRegion.bottomRight.y * scaleY)), 0,
                   static_cast<int>(height));

    // Skip clipping if the region covers the entire texture.
    if (x0 > 0 || y0 > 0 || x1 < static_cast<int>(width) || y1 < static_cast<int>(height)) {
      const wgpu::Device& dev = device_.device();

      // Allocate destination with RenderAttachment (for clear) + CopyDst.
      wgpu::TextureDescriptor td{};
      td.label = wgpuLabel("FilterClipOutput");
      td.size = {width, height, 1};
      td.format = kFormat;
      td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                 wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
      td.mipLevelCount = 1;
      td.sampleCount = 1;
      td.dimension = wgpu::TextureDimension::_2D;
      wgpu::Texture clipped = dev.createTexture(td);

      // Clear the destination to transparent via a render pass.
      wgpu::TextureView clippedView = clipped.createView();
      wgpu::RenderPassColorAttachment colorAtt{};
      colorAtt.view = clippedView;
      colorAtt.loadOp = wgpu::LoadOp::Clear;
      colorAtt.storeOp = wgpu::StoreOp::Store;
      colorAtt.clearValue = {0.0, 0.0, 0.0, 0.0};

      wgpu::RenderPassDescriptor rpDesc{};
      rpDesc.label = wgpuLabel("FilterClipClear");
      rpDesc.colorAttachmentCount = 1;
      rpDesc.colorAttachments = &colorAtt;

      wgpu::CommandEncoderDescriptor ceDesc{};
      ceDesc.label = wgpuLabel("FilterClipEncoder");
      wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

      wgpu::RenderPassEncoder rp = encoder.beginRenderPass(rpDesc);
      rp.end();

      // Copy just the filter region from the result into the cleared texture.
      if (x1 > x0 && y1 > y0) {
        wgpu::TexelCopyTextureInfo srcCopy{};
        srcCopy.texture = currentBuffer;
        srcCopy.origin = {static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), 0};

        wgpu::TexelCopyTextureInfo dstCopy{};
        dstCopy.texture = clipped;
        dstCopy.origin = {static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), 0};

        wgpu::Extent3D extent = {static_cast<uint32_t>(x1 - x0),
                                 static_cast<uint32_t>(y1 - y0), 1};
        encoder.copyTextureToTexture(srcCopy, dstCopy, extent);
      }

      wgpu::CommandBuffer cmdBuf = encoder.finish();
      device_.queue().submit(1, &cmdBuf);
      currentBuffer = clipped;
    }
  }

  return currentBuffer;
}

wgpu::Texture GeodeFilterEngine::applyGaussianBlur(const wgpu::Texture& input, double stdDeviationX,
                                                   double stdDeviationY, uint32_t edgeMode) {
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  if (stdDeviationX <= 0.0 && stdDeviationY <= 0.0) {
    return input;
  }

  wgpu::Texture afterHorizontal = input;
  if (stdDeviationX > 0.0) {
    afterHorizontal =
        runBlurPass(input, width, height, static_cast<float>(stdDeviationX), /*axis=*/0, edgeMode);
  }

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

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "GaussianBlurPass");

  BlurParams params{};
  params.stdDeviation = stdDeviation;
  params.axis = axis;
  params.edgeMode = edgeMode;
  params.pad = 0;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "BlurParamsUniform");

  dispatchInputOutputUniform(device_, blurBindGroupLayout_, gaussianBlurPipeline_, input, output,
                             uniformBuffer, sizeof(BlurParams), "GaussianBlurPass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyOffset(
    const wgpu::Texture& input, const svg::components::filter_primitive::Offset& primitive) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  // Zero offset → passthrough.
  if (primitive.dx == 0.0 && primitive.dy == 0.0) {
    return input;
  }

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterOffsetOutput");

  OffsetParams params{};
  params.dx = static_cast<float>(primitive.dx);
  params.dy = static_cast<float>(primitive.dy);
  params.edgeMode = 0;  // None (transparent OOB).
  params.pad = 0;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "OffsetParamsUniform");

  dispatchInputOutputUniform(device_, offsetBindGroupLayout_, offsetPipeline_, input, output,
                             uniformBuffer, sizeof(OffsetParams), "FilterOffsetPass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyColorMatrix(
    const wgpu::Texture& input,
    const svg::components::filter_primitive::ColorMatrix& primitive) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterColorMatrixOutput");

  ColorMatrixParams params = buildColorMatrix(primitive);

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "ColorMatrixParamsUniform");

  dispatchInputOutputUniform(device_, colorMatrixBindGroupLayout_, colorMatrixPipeline_, input,
                             output, uniformBuffer, sizeof(ColorMatrixParams),
                             "FilterColorMatrixPass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyFlood(
    uint32_t width, uint32_t height,
    const svg::components::filter_primitive::Flood& primitive) {
  const wgpu::Device& dev = device_.device();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterFloodOutput");

  // Resolve the flood color: CSS color → RGBA, then premultiply.  The filter
  // pipeline operates in premultiplied alpha (consistent with TinySkia and the
  // Porter-Duff compositing in feMerge).
  const css::RGBA rgba = primitive.floodColor.asRGBA();
  const float alpha = (static_cast<float>(rgba.a) / 255.0f) *
                       static_cast<float>(std::clamp(primitive.floodOpacity, 0.0, 1.0));

  FloodParams params{};
  params.color[0] = (static_cast<float>(rgba.r) / 255.0f) * alpha;
  params.color[1] = (static_cast<float>(rgba.g) / 255.0f) * alpha;
  params.color[2] = (static_cast<float>(rgba.b) / 255.0f) * alpha;
  params.color[3] = alpha;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "FloodParamsUniform");

  // Flood has no input texture — only output + uniform.
  wgpu::TextureView outputView = output.createView();

  wgpu::BindGroupEntry bgEntries[2]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = outputView;
  bgEntries[1].binding = 1;
  bgEntries[1].buffer = uniformBuffer;
  bgEntries[1].offset = 0;
  bgEntries[1].size = sizeof(FloodParams);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterFloodBindGroup");
  bgDesc.layout = floodBindGroupLayout_;
  bgDesc.entryCount = 2;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel("FilterFloodEncoder");
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterFloodPass");
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(floodPipeline_);
  pass.setBindGroup(0, bindGroup, 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.end();

  wgpu::CommandBuffer cmdBuf = encoder.finish();
  device_.queue().submit(1, &cmdBuf);

  return output;
}

wgpu::Texture GeodeFilterEngine::applyMerge(
    const svg::components::FilterNode& node,
    const std::unordered_map<std::string, wgpu::Texture>& namedBuffers,
    const wgpu::Texture& currentBuffer, const wgpu::Texture& sourceGraphic) {
  const uint32_t width = currentBuffer.getWidth();
  const uint32_t height = currentBuffer.getHeight();

  if (node.inputs.empty()) {
    return currentBuffer;
  }

  // Resolve first input as the initial accumulator.
  wgpu::Texture accumulator =
      resolveInput(node.inputs[0], namedBuffers, currentBuffer, sourceGraphic);

  // Alpha-over composite each subsequent input on top.
  for (size_t i = 1; i < node.inputs.size(); ++i) {
    wgpu::Texture src = resolveInput(node.inputs[i], namedBuffers, currentBuffer, sourceGraphic);
    accumulator = runMergePass(src, accumulator, width, height);
  }

  return accumulator;
}

wgpu::Texture GeodeFilterEngine::runMergePass(const wgpu::Texture& src, const wgpu::Texture& dst,
                                              uint32_t width, uint32_t height) {
  const wgpu::Device& dev = device_.device();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterMergeOutput");

  wgpu::TextureView srcView = src.createView();
  wgpu::TextureView dstView = dst.createView();
  wgpu::TextureView outputView = output.createView();

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = srcView;
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = dstView;
  bgEntries[2].binding = 2;
  bgEntries[2].textureView = outputView;

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterMergeBindGroup");
  bgDesc.layout = mergeBindGroupLayout_;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel("FilterMergeEncoder");
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterMergePass");
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(mergePipeline_);
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
