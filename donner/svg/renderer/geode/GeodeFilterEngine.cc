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

/// Uniform buffer layout matching the WGSL `CompositeParams` struct.
struct CompositeParams {
  uint32_t op;    // Operator index (0..6).
  uint32_t pad0;
  uint32_t pad1;
  uint32_t pad2;
  float k1;       // Arithmetic coefficient k1.
  float k2;       // Arithmetic coefficient k2.
  float k3;       // Arithmetic coefficient k3.
  float k4;       // Arithmetic coefficient k4.
};

/// Uniform buffer layout matching the WGSL `BlendParams` struct.
struct BlendParams {
  uint32_t mode;  // Blend mode index (0..15).
  uint32_t pad0;
  uint32_t pad1;
  uint32_t pad2;
};

/// Uniform buffer layout matching the WGSL `MorphologyParams` struct.
struct MorphologyParams {
  int32_t radiusX;
  int32_t radiusY;
  uint32_t op;  // 0 = erode, 1 = dilate.
  uint32_t pad;
};

/// GPU storage buffer layout matching the WGSL `ConvolveParams` struct.
/// Uses storage (not uniform) because WGSL uniform array<f32,N> has 16-byte element stride.
struct ConvolveParams {
  int32_t orderX;
  int32_t orderY;
  int32_t targetX;
  int32_t targetY;
  float divisor;
  float bias;
  uint32_t edgeMode;
  uint32_t preserveAlpha;
  float kernel[25];  // Row-major kernel values (max 5×5).
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

/// Helper to create a pipeline with a two-input (in1, in2, output, uniform) bind group layout.
/// Used by feComposite and feBlend pipelines.
struct TwoInputUniformPipeline {
  wgpu::BindGroupLayout bindGroupLayout;
  wgpu::ComputePipeline pipeline;
};

TwoInputUniformPipeline createTwoInputUniformPipeline(const wgpu::Device& dev, const char* label,
                                                      wgpu::ShaderModule shaderModule,
                                                      size_t uniformSize) {
  wgpu::BindGroupLayoutEntry entries[4]{};

  // binding 0: in1 (texture_2d)
  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[0].texture.multisampled = false;

  // binding 1: in2 (texture_2d)
  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[1].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[1].texture.multisampled = false;

  // binding 2: output (storage texture)
  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Compute;
  entries[2].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  entries[2].storageTexture.format = kFormat;
  entries[2].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  // binding 3: uniform buffer
  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Compute;
  entries[3].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[3].buffer.minBindingSize = uniformSize;

  std::string bglLabel = std::string(label) + "BGL";
  wgpu::BindGroupLayoutDescriptor bglDesc{};
  bglDesc.label = wgpuLabel(bglLabel.c_str());
  bglDesc.entryCount = 4;
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

/// Dispatch a compute shader with a two-input (in1, in2, output, uniform) bind group.
void dispatchTwoInputUniform(GeodeDevice& device, const wgpu::BindGroupLayout& bgl,
                             const wgpu::ComputePipeline& pipeline, const wgpu::Texture& in1,
                             const wgpu::Texture& in2, const wgpu::Texture& output,
                             const wgpu::Buffer& uniformBuffer, size_t uniformSize,
                             const char* label) {
  const wgpu::Device& dev = device.device();
  const uint32_t width = output.getWidth();
  const uint32_t height = output.getHeight();

  wgpu::TextureView in1View = in1.createView();
  wgpu::TextureView in2View = in2.createView();
  wgpu::TextureView outputView = output.createView();

  wgpu::BindGroupEntry bgEntries[4]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = in1View;
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = in2View;
  bgEntries[2].binding = 2;
  bgEntries[2].textureView = outputView;
  bgEntries[3].binding = 3;
  bgEntries[3].buffer = uniformBuffer;
  bgEntries[3].offset = 0;
  bgEntries[3].size = uniformSize;

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel(label);
  bgDesc.layout = bgl;
  bgDesc.entryCount = 4;
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

/// Map a ConvolveMatrix EdgeMode to the shader's uint.
uint32_t toConvolveEdgeMode(svg::components::filter_primitive::ConvolveMatrix::EdgeMode mode) {
  using EM = svg::components::filter_primitive::ConvolveMatrix::EdgeMode;
  switch (mode) {
    case EM::Duplicate: return 0;
    case EM::Wrap: return 1;
    case EM::None: return 2;
  }
  return 0;
}

/// Build a 256-entry uint8 LUT for one channel's feComponentTransfer function.
void buildChannelLut(const svg::components::filter_primitive::ComponentTransfer::Func& func,
                     uint32_t* outLut) {
  using FT = svg::components::filter_primitive::ComponentTransfer::FuncType;
  for (int i = 0; i < 256; ++i) {
    const double c = static_cast<double>(i) / 255.0;
    double result = c;

    switch (func.type) {
      case FT::Identity: result = c; break;
      case FT::Table: {
        const auto& tv = func.tableValues;
        if (tv.size() >= 2) {
          const double k = c * static_cast<double>(tv.size() - 1);
          const int idx = std::min(static_cast<int>(k), static_cast<int>(tv.size() - 2));
          const double frac = k - static_cast<double>(idx);
          result = tv[idx] * (1.0 - frac) + tv[idx + 1] * frac;
        } else if (tv.size() == 1) {
          result = tv[0];
        }
        break;
      }
      case FT::Discrete: {
        const auto& tv = func.tableValues;
        if (!tv.empty()) {
          const int idx = std::min(static_cast<int>(c * static_cast<double>(tv.size())),
                                   static_cast<int>(tv.size() - 1));
          result = tv[idx];
        }
        break;
      }
      case FT::Linear: result = func.slope * c + func.intercept; break;
      case FT::Gamma:
        result = func.amplitude * std::pow(c, func.exponent) + func.offset;
        break;
    }

    result = std::clamp(result, 0.0, 1.0);
    outLut[i] = static_cast<uint32_t>(std::round(result * 255.0));
  }
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

  // --- feComposite Porter-Duff pipeline (two inputs + output + uniform) ---
  {
    auto [bgl, pipeline] = createTwoInputUniformPipeline(
        dev, "FilterComposite", createFilterCompositeShader(dev), sizeof(CompositeParams));
    compositeBindGroupLayout_ = bgl;
    compositePipeline_ = pipeline;
  }

  // --- feBlend W3C blend-mode pipeline (two inputs + output + uniform) ---
  {
    auto [bgl, pipeline] = createTwoInputUniformPipeline(
        dev, "FilterBlend", createFilterBlendShader(dev), sizeof(BlendParams));
    blendBindGroupLayout_ = bgl;
    blendPipeline_ = pipeline;
  }

  // --- feMorphology pipeline (input + output + uniform) ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "FilterMorphology", createFilterMorphologyShader(dev), sizeof(MorphologyParams));
    morphologyBindGroupLayout_ = bgl;
    morphologyPipeline_ = pipeline;
  }

  // --- feComponentTransfer pipeline (input + output + storage buffer for LUT) ---
  {
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
    entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[2].buffer.minBindingSize = 1024 * sizeof(uint32_t);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterComponentTransferBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    componentTransferBindGroupLayout_ = dev.createBindGroupLayout(bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterComponentTransferPipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {componentTransferBindGroupLayout_};
    plDesc.bindGroupLayouts = layouts;
    wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterComponentTransferPipeline");
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = createFilterComponentTransferShader(dev);
    cpDesc.compute.entryPoint = wgpuLabel("main");
    componentTransferPipeline_ = dev.createComputePipeline(cpDesc);
  }

  // --- feConvolveMatrix pipeline (input + output + storage buffer for params) ---
  // Uses ReadOnlyStorage instead of Uniform because WGSL uniform arrays
  // have 16-byte element stride, making array<f32, 25> 400 bytes vs 100.
  {
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
    entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[2].buffer.minBindingSize = sizeof(ConvolveParams);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterConvolveMatrixBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    convolveMatrixBindGroupLayout_ = dev.createBindGroupLayout(bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterConvolveMatrixPipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {convolveMatrixBindGroupLayout_};
    plDesc.bindGroupLayouts = layouts;
    wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterConvolveMatrixPipeline");
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = createFilterConvolveMatrixShader(dev);
    cpDesc.compute.entryPoint = wgpuLabel("main");
    convolveMatrixPipeline_ = dev.createComputePipeline(cpDesc);
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
    } else if (const auto* composite =
                   std::get_if<filter_primitive::Composite>(&node.primitive)) {
      // Resolve second input (in2/backdrop).
      wgpu::Texture in2Tex = inputTex;
      if (node.inputs.size() >= 2) {
        in2Tex = resolveInput(node.inputs[1], namedBuffers, currentBuffer, sourceGraphic);
      }
      outputTex = applyComposite(inputTex, in2Tex, *composite);
    } else if (const auto* blend = std::get_if<filter_primitive::Blend>(&node.primitive)) {
      // Resolve second input (in2/backdrop).
      wgpu::Texture in2Tex = inputTex;
      if (node.inputs.size() >= 2) {
        in2Tex = resolveInput(node.inputs[1], namedBuffers, currentBuffer, sourceGraphic);
      }
      outputTex = applyBlend(inputTex, in2Tex, *blend);
    } else if (const auto* morph = std::get_if<filter_primitive::Morphology>(&node.primitive)) {
      const int rx = static_cast<int>(std::round(toPixelX(morph->radiusX)));
      const int ry = static_cast<int>(std::round(toPixelY(morph->radiusY)));
      outputTex = applyMorphology(inputTex, *morph, rx, ry);
    } else if (const auto* ct =
                   std::get_if<filter_primitive::ComponentTransfer>(&node.primitive)) {
      outputTex = applyComponentTransfer(inputTex, *ct);
    } else if (const auto* conv =
                   std::get_if<filter_primitive::ConvolveMatrix>(&node.primitive)) {
      outputTex = applyConvolveMatrix(inputTex, *conv);
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

wgpu::Texture GeodeFilterEngine::applyComposite(
    const wgpu::Texture& in1, const wgpu::Texture& in2,
    const svg::components::filter_primitive::Composite& primitive) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = in1.getWidth();
  const uint32_t height = in1.getHeight();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterCompositeOutput");

  // Map the Composite::Operator enum to the shader's uint index.
  using Op = svg::components::filter_primitive::Composite::Operator;
  uint32_t opIndex = 0;
  switch (primitive.op) {
    case Op::Over: opIndex = 0; break;
    case Op::In: opIndex = 1; break;
    case Op::Out: opIndex = 2; break;
    case Op::Atop: opIndex = 3; break;
    case Op::Xor: opIndex = 4; break;
    case Op::Lighter: opIndex = 5; break;
    case Op::Arithmetic: opIndex = 6; break;
  }

  CompositeParams params{};
  params.op = opIndex;
  params.pad0 = 0;
  params.pad1 = 0;
  params.pad2 = 0;
  params.k1 = static_cast<float>(primitive.k1);
  params.k2 = static_cast<float>(primitive.k2);
  params.k3 = static_cast<float>(primitive.k3);
  params.k4 = static_cast<float>(primitive.k4);

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "CompositeParamsUniform");

  dispatchTwoInputUniform(device_, compositeBindGroupLayout_, compositePipeline_, in1, in2, output,
                          uniformBuffer, sizeof(CompositeParams), "FilterCompositePass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyBlend(
    const wgpu::Texture& in1, const wgpu::Texture& in2,
    const svg::components::filter_primitive::Blend& primitive) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = in1.getWidth();
  const uint32_t height = in1.getHeight();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterBlendOutput");

  BlendParams params{};
  params.mode = static_cast<uint32_t>(primitive.mode);
  params.pad0 = 0;
  params.pad1 = 0;
  params.pad2 = 0;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "BlendParamsUniform");

  dispatchTwoInputUniform(device_, blendBindGroupLayout_, blendPipeline_, in1, in2, output,
                          uniformBuffer, sizeof(BlendParams), "FilterBlendPass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyMorphology(
    const wgpu::Texture& input,
    const svg::components::filter_primitive::Morphology& primitive, int pixelRadiusX,
    int pixelRadiusY) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  // Zero or negative radius → passthrough.
  if (pixelRadiusX <= 0 && pixelRadiusY <= 0) {
    return input;
  }

  // Cap total kernel samples at 63×63 = 3969.
  constexpr int kMaxRadius = 31;
  if (pixelRadiusX > kMaxRadius || pixelRadiusY > kMaxRadius) {
    // TODO(geode): Support larger morphology kernels via separable passes.
    if (verbose_) {
      std::cerr << "GeodeFilterEngine: feMorphology radius ("
                << pixelRadiusX << "×" << pixelRadiusY
                << ") exceeds cap (" << kMaxRadius << "); passthrough\n";
    }
    return input;
  }

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterMorphologyOutput");

  using Op = svg::components::filter_primitive::Morphology::Operator;
  MorphologyParams params{};
  params.radiusX = pixelRadiusX;
  params.radiusY = pixelRadiusY;
  params.op = primitive.op == Op::Dilate ? 1u : 0u;
  params.pad = 0;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "MorphologyParamsUniform");

  dispatchInputOutputUniform(device_, morphologyBindGroupLayout_, morphologyPipeline_, input, output,
                             uniformBuffer, sizeof(MorphologyParams), "FilterMorphologyPass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyComponentTransfer(
    const wgpu::Texture& input,
    const svg::components::filter_primitive::ComponentTransfer& primitive) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  wgpu::Texture output =
      createIntermediateTexture(dev, width, height, "FilterComponentTransferOutput");

  // Build 4 × 256-entry LUTs (R, G, B, A) as uint32 array (1024 entries).
  std::array<uint32_t, 1024> lutData{};
  buildChannelLut(primitive.funcR, &lutData[0]);
  buildChannelLut(primitive.funcG, &lutData[256]);
  buildChannelLut(primitive.funcB, &lutData[512]);
  buildChannelLut(primitive.funcA, &lutData[768]);

  // Upload as a storage buffer.
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("ComponentTransferLUT");
  bufDesc.size = lutData.size() * sizeof(uint32_t);
  bufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer lutBuffer = dev.createBuffer(bufDesc);
  device_.queue().writeBuffer(lutBuffer, 0, lutData.data(), bufDesc.size);

  // Build bind group.
  wgpu::TextureView inputView = input.createView();
  wgpu::TextureView outputView = output.createView();

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = inputView;
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = outputView;
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = lutBuffer;
  bgEntries[2].offset = 0;
  bgEntries[2].size = bufDesc.size;

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterComponentTransferBindGroup");
  bgDesc.layout = componentTransferBindGroupLayout_;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel("FilterComponentTransferEncoder");
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterComponentTransferPass");
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(componentTransferPipeline_);
  pass.setBindGroup(0, bindGroup, 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.end();

  wgpu::CommandBuffer cmdBuf = encoder.finish();
  device_.queue().submit(1, &cmdBuf);
  return output;
}

wgpu::Texture GeodeFilterEngine::applyConvolveMatrix(
    const wgpu::Texture& input,
    const svg::components::filter_primitive::ConvolveMatrix& primitive) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  // Cap kernel at 5×5.
  if (primitive.orderX > 5 || primitive.orderY > 5 || primitive.orderX <= 0 ||
      primitive.orderY <= 0) {
    // TODO(geode): Support larger convolution kernels.
    if (verbose_) {
      std::cerr << "GeodeFilterEngine: feConvolveMatrix kernel ("
                << primitive.orderX << "×" << primitive.orderY
                << ") outside 1..5 range; passthrough\n";
    }
    return input;
  }

  wgpu::Texture output =
      createIntermediateTexture(dev, width, height, "FilterConvolveMatrixOutput");

  // Compute effective divisor (default = sum of kernel values, or 1 if sum is 0).
  double divisor = 1.0;
  if (primitive.divisor.has_value()) {
    divisor = primitive.divisor.value();
    if (divisor == 0.0) {
      divisor = 1.0;
    }
  } else {
    double sum = 0.0;
    for (double v : primitive.kernelMatrix) {
      sum += v;
    }
    divisor = (sum != 0.0) ? sum : 1.0;
  }

  const int targetX = primitive.targetX.value_or(primitive.orderX / 2);
  const int targetY = primitive.targetY.value_or(primitive.orderY / 2);

  ConvolveParams params{};
  params.orderX = primitive.orderX;
  params.orderY = primitive.orderY;
  params.targetX = targetX;
  params.targetY = targetY;
  params.divisor = static_cast<float>(divisor);
  params.bias = static_cast<float>(primitive.bias);
  params.edgeMode = toConvolveEdgeMode(primitive.edgeMode);
  params.preserveAlpha = primitive.preserveAlpha ? 1u : 0u;

  // Fill kernel array (row-major, max 25 entries).
  std::fill(std::begin(params.kernel), std::end(params.kernel), 0.0f);
  const int count = std::min(static_cast<int>(primitive.kernelMatrix.size()),
                             primitive.orderX * primitive.orderY);
  for (int i = 0; i < count && i < 25; ++i) {
    params.kernel[i] = static_cast<float>(primitive.kernelMatrix[i]);
  }

  // Upload as a storage buffer (not uniform — array<f32,25> has 16-byte stride in uniform).
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("ConvolveMatrixParamsStorage");
  bufDesc.size = sizeof(ConvolveParams);
  bufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer paramsBuffer = dev.createBuffer(bufDesc);
  device_.queue().writeBuffer(paramsBuffer, 0, &params, sizeof(params));

  // Build bind group.
  wgpu::TextureView inputView = input.createView();
  wgpu::TextureView outputView = output.createView();

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = inputView;
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = outputView;
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = paramsBuffer;
  bgEntries[2].offset = 0;
  bgEntries[2].size = sizeof(ConvolveParams);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterConvolveMatrixBindGroup");
  bgDesc.layout = convolveMatrixBindGroupLayout_;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel("FilterConvolveMatrixEncoder");
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterConvolveMatrixPass");
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(convolveMatrixPipeline_);
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
