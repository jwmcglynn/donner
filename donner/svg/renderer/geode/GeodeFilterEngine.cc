#include "donner/svg/renderer/geode/GeodeFilterEngine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <unordered_map>
#include <variant>

#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/base/MathUtils.h"
#include "donner/base/RelativeLengthMetrics.h"
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

/// GPU storage buffer layout matching the WGSL `TurbulenceParams` struct.
struct TurbulenceParams {
  float baseFreqX;
  float baseFreqY;
  int32_t numOctaves;
  int32_t seed;
  uint32_t stitchTiles;
  uint32_t typeFlag;
  float tileWidth;
  float tileHeight;
  float filterFromDeviceA;
  float filterFromDeviceB;
  float filterFromDeviceC;
  float filterFromDeviceD;
};

/// Pre-computed permutation + gradient tables for feTurbulence.
/// Matches the SVG spec's Perlin noise algorithm (Park-Miller LCG + Fisher-Yates shuffle).
/// Layout matches the WGSL `TurbulenceTables` struct.
constexpr int kTurbBLen = 256;
constexpr int kTurbBLenPlus2 = kTurbBLen + 2;  // 258
constexpr int kTurbTableSize = kTurbBLen + kTurbBLenPlus2;  // 514

struct TurbulenceTables {
  int32_t lattice[kTurbTableSize];        // Permutation table (514 entries).
  float gradX[4 * kTurbTableSize];        // Gradient X: [channel * 514 + index].
  float gradY[4 * kTurbTableSize];        // Gradient Y: [channel * 514 + index].
};

// Park-Miller LCG constants matching tiny-skia/resvg.
constexpr long kRandM = 2147483647;  // 2^31 - 1  // NOLINT
constexpr long kRandA = 16807;                     // NOLINT
constexpr long kRandQ = 127773;                    // NOLINT
constexpr long kRandR = 2836;                      // NOLINT

/// Park-Miller LCG step (Schrage's method to avoid overflow).
long turbulenceRandom(long seed) {  // NOLINT
  long result = kRandA * (seed % kRandQ) - kRandR * (seed / kRandQ);  // NOLINT
  if (result <= 0) {
    result += kRandM;
  }
  return result;
}

/// Generate permutation + gradient tables from seed, matching tiny-skia exactly.
void generateTurbulenceTables(double seedVal, TurbulenceTables& tables) {
  // Seed clamping matching resvg.
  long seed;  // NOLINT
  if (seedVal <= 0) {
    seed = -(static_cast<long>(seedVal)) % (kRandM - 1) + 1;  // NOLINT
  } else if (seedVal > kRandM - 1) {
    seed = kRandM - 1;
  } else {
    seed = static_cast<long>(seedVal);  // NOLINT
  }

  // Temporary double-precision gradient storage during generation.
  double gradient[4][kTurbTableSize][2] = {};

  // Generate gradient tables for all 4 channels.
  for (int ch = 0; ch < 4; ch++) {
    for (int k = 0; k < kTurbBLen; k++) {
      if (ch == 0) {
        tables.lattice[k] = k;
      }

      seed = turbulenceRandom(seed);
      gradient[ch][k][0] =
          static_cast<double>((seed % (kTurbBLen + kTurbBLen)) - kTurbBLen) / kTurbBLen;
      seed = turbulenceRandom(seed);
      gradient[ch][k][1] =
          static_cast<double>((seed % (kTurbBLen + kTurbBLen)) - kTurbBLen) / kTurbBLen;

      // Normalize to unit length.
      const double mag =
          std::sqrt(gradient[ch][k][0] * gradient[ch][k][0] + gradient[ch][k][1] * gradient[ch][k][1]);
      if (mag > 1e-10) {
        gradient[ch][k][0] /= mag;
        gradient[ch][k][1] /= mag;
      }
    }
  }

  // Fisher-Yates shuffle of the lattice selector.
  for (int i = kTurbBLen - 1; i > 0; i--) {
    seed = turbulenceRandom(seed);
    const int target = static_cast<int>(seed % kTurbBLen);
    std::swap(tables.lattice[i], tables.lattice[target]);
  }

  // Duplicate entries for wrapping.
  for (int i = 0; i < kTurbBLenPlus2; i++) {
    tables.lattice[kTurbBLen + i] = tables.lattice[i];
    for (int ch = 0; ch < 4; ch++) {
      gradient[ch][kTurbBLen + i][0] = gradient[ch][i][0];
      gradient[ch][kTurbBLen + i][1] = gradient[ch][i][1];
    }
  }

  // Convert to float SOA layout matching WGSL struct.
  for (int ch = 0; ch < 4; ch++) {
    for (int idx = 0; idx < kTurbTableSize; idx++) {
      tables.gradX[ch * kTurbTableSize + idx] = static_cast<float>(gradient[ch][idx][0]);
      tables.gradY[ch * kTurbTableSize + idx] = static_cast<float>(gradient[ch][idx][1]);
    }
  }
}

/// Uniform buffer layout matching the WGSL `DisplacementParams` struct.
struct DisplacementParams {
  float scale;
  uint32_t xChannel;
  uint32_t yChannel;
  uint32_t pad;
};

/// GPU storage buffer layout matching the WGSL diffuse `LightingParams` struct.
struct DiffuseLightingParams {
  float surfaceScale;
  float diffuseConstant;
  float pad0;
  float pad1;

  float lightR;
  float lightG;
  float lightB;
  uint32_t lightType;

  float azimuthRad;
  float elevationRad;

  float lightX;
  float lightY;
  float lightZ;

  float pointsAtX;
  float pointsAtY;
  float pointsAtZ;
  float spotExponent;
  float cosConeAngle;

  float pad2;
  float pad3;
  float pad4;
};

/// Uniform buffer layout matching the WGSL `DropShadowParams` struct.
struct DropShadowParams {
  float color[4];  // Flood color, straight alpha.
  float dx;
  float dy;
  uint32_t pad0;
  uint32_t pad1;
};

/// Uniform buffer layout matching the WGSL `ImageParams` struct.
/// Row-major 2×3 transform: src = M * (dst_pixel + 0.5, 1).
struct ImageParams {
  float m00;
  float m01;
  float m02;
  float m10;
  float m11;
  float m12;
  uint32_t pad0;
  uint32_t pad1;
};

/// Uniform buffer layout matching the WGSL `TileParams` struct.
struct TileParams {
  int32_t srcX;
  int32_t srcY;
  int32_t srcW;
  int32_t srcH;
};

/// Uniform buffer layout matching the WGSL `SubregionClipParams` struct.
struct SubregionClipParams {
  float invA;
  float invB;
  float invC;
  float invD;
  float invE;
  float invF;
  float usrX0;
  float usrY0;
  float usrX1;
  float usrY1;
  uint32_t pad0;
  uint32_t pad1;
};

/// Uniform buffer layout for the sRGB↔linearRGB color space conversion shader.
struct ColorSpaceConvertParams {
  uint32_t direction;  // 0 = sRGB→linear, 1 = linear→sRGB.
  uint32_t pad0;
  uint32_t pad1;
  uint32_t pad2;
};

/// GPU storage buffer layout matching the WGSL specular `LightingParams` struct.
struct SpecularLightingParams {
  float surfaceScale;
  float specularConstant;
  float specularExponent;
  float pad0;

  float lightR;
  float lightG;
  float lightB;
  uint32_t lightType;

  float azimuthRad;
  float elevationRad;

  float lightX;
  float lightY;
  float lightZ;

  float pointsAtX;
  float pointsAtY;
  float pointsAtZ;
  float spotExponent;
  float cosConeAngle;

  float pad1;
  float pad2;
  float pad3;
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
      if (primitive.values.size() == 20) {
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

  // --- feTurbulence pipeline (output + params buffer + tables buffer) ---
  {
    wgpu::BindGroupLayoutEntry entries[3]{};

    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
    entries[0].storageTexture.format = kFormat;
    entries[0].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[1].buffer.minBindingSize = sizeof(TurbulenceParams);

    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Compute;
    entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[2].buffer.minBindingSize = sizeof(TurbulenceTables);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterTurbulenceBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    turbulenceBindGroupLayout_ = dev.createBindGroupLayout(bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterTurbulencePipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {turbulenceBindGroupLayout_};
    plDesc.bindGroupLayouts = layouts;
    wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterTurbulencePipeline");
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = createFilterTurbulenceShader(dev);
    cpDesc.compute.entryPoint = wgpuLabel("main");
    turbulencePipeline_ = dev.createComputePipeline(cpDesc);
  }

  // --- feDisplacementMap pipeline (two inputs + output + uniform) ---
  {
    auto [bgl, pipeline] = createTwoInputUniformPipeline(
        dev, "FilterDisplacementMap", createFilterDisplacementMapShader(dev),
        sizeof(DisplacementParams));
    displacementMapBindGroupLayout_ = bgl;
    displacementMapPipeline_ = pipeline;
  }

  // --- feDiffuseLighting pipeline (input + output + storage buffer) ---
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
    entries[2].buffer.minBindingSize = sizeof(DiffuseLightingParams);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterDiffuseLightingBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    diffuseLightingBindGroupLayout_ = dev.createBindGroupLayout(bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterDiffuseLightingPipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {diffuseLightingBindGroupLayout_};
    plDesc.bindGroupLayouts = layouts;
    wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterDiffuseLightingPipeline");
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = createFilterDiffuseLightingShader(dev);
    cpDesc.compute.entryPoint = wgpuLabel("main");
    diffuseLightingPipeline_ = dev.createComputePipeline(cpDesc);
  }

  // --- feSpecularLighting pipeline (input + output + storage buffer) ---
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
    entries[2].buffer.minBindingSize = sizeof(SpecularLightingParams);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterSpecularLightingBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    specularLightingBindGroupLayout_ = dev.createBindGroupLayout(bglDesc);

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterSpecularLightingPipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {specularLightingBindGroupLayout_};
    plDesc.bindGroupLayouts = layouts;
    wgpu::PipelineLayout pipelineLayout = dev.createPipelineLayout(plDesc);

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterSpecularLightingPipeline");
    cpDesc.layout = pipelineLayout;
    cpDesc.compute.module = createFilterSpecularLightingShader(dev);
    cpDesc.compute.entryPoint = wgpuLabel("main");
    specularLightingPipeline_ = dev.createComputePipeline(cpDesc);
  }

  // --- feDropShadow compose pipeline (two inputs + output + uniform) ---
  {
    auto [bgl, pipeline] = createTwoInputUniformPipeline(
        dev, "FilterDropShadow", createFilterDropShadowShader(dev), sizeof(DropShadowParams));
    dropShadowBindGroupLayout_ = bgl;
    dropShadowPipeline_ = pipeline;
  }

  // --- feImage placement pipeline (input + output + uniform) ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "FilterImage", createFilterImageShader(dev), sizeof(ImageParams));
    imageBindGroupLayout_ = bgl;
    imagePipeline_ = pipeline;
  }

  // --- feTile wraparound pipeline (input + output + uniform) ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "FilterTile", createFilterTileShader(dev), sizeof(TileParams));
    tileBindGroupLayout_ = bgl;
    tilePipeline_ = pipeline;
  }

  // --- Per-primitive subregion clipping pipeline (input + output + uniform) ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "FilterSubregionClip", createFilterSubregionClipShader(dev),
        sizeof(SubregionClipParams));
    subregionClipBindGroupLayout_ = bgl;
    subregionClipPipeline_ = pipeline;
  }

  // --- sRGB↔linearRGB color space conversion pipeline ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "FilterColorSpaceConvert", createFilterColorSpaceConvertShader(dev),
        sizeof(ColorSpaceConvertParams));
    colorSpaceConvertBindGroupLayout_ = bgl;
    colorSpaceConvertPipeline_ = pipeline;
  }
}

GeodeFilterEngine::~GeodeFilterEngine() = default;

wgpu::Texture GeodeFilterEngine::execute(const svg::components::FilterGraph& graph,
                                         const wgpu::Texture& sourceGraphic,
                                         const Box2d& filterRegion,
                                         const Transform2d& deviceFromFilter) {
  using namespace svg::components;

  std::unordered_map<std::string, wgpu::Texture> namedBuffers;
  wgpu::Texture currentBuffer = sourceGraphic;

  // Derive per-axis scale factors from the full CTM's basis vectors so
  // that rotation/skew in the ancestor transform is accounted for.
  // This matches the CPU FilterGraphExecutor's decomposition.
  const Vector2d transformedXAxis = deviceFromFilter.transformVector(Vector2d(1.0, 0.0));
  const double scaleX = std::max(transformedXAxis.length(), 1e-12);
  const double determinant = deviceFromFilter.determinant();
  const double scaleY =
      NearZero(scaleX, 1e-12) ? std::abs(deviceFromFilter.data[3])
                               : std::max(std::abs(determinant) / scaleX, 1e-12);

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

  // Transform a user-space offset vector through the full CTM so that
  // rotation/skew is preserved (used by feOffset, feDropShadow).
  auto toPixelOffset = [&](double dx, double dy) -> Vector2d {
    const Vector2d userOffset = isOBB ? Vector2d(dx * bboxW, dy * bboxH) : Vector2d(dx, dy);
    return deviceFromFilter.transformVector(userOffset);
  };

  // Bounding-box origin for OBB position resolution.
  const double bboxX =
      graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->topLeft.x : 0.0;
  const double bboxY =
      graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->topLeft.y : 0.0;

  // Inverse CTM for rotation-aware subregion clipping.
  const Transform2d filterFromDevice = deviceFromFilter.inverse();

  // Reference bounds for Length::toPixels in the non-OBB case.
  const Box2d primitiveUnitsBounds = [&]() -> Box2d {
    if (isOBB) {
      return graph.elementBoundingBox.value_or(Box2d(Vector2d(0.0, 0.0), Vector2d(1.0, 1.0)));
    }
    const double userW = NearZero(scaleX, 1e-12)
                             ? static_cast<double>(sourceGraphic.getWidth())
                             : static_cast<double>(sourceGraphic.getWidth()) / scaleX;
    const double userH = NearZero(scaleY, 1e-12)
                             ? static_cast<double>(sourceGraphic.getHeight())
                             : static_cast<double>(sourceGraphic.getHeight()) / scaleY;
    return Box2d::FromXYWH(0.0, 0.0, userW, userH);
  }();

  // Resolve a primitive subregion length to user-space position.
  auto resolvePrimitivePosition = [&](const Lengthd& len, Lengthd::Extent extent, double origin,
                                      double bboxDim) -> double {
    if (isOBB && len.unit == Lengthd::Unit::None) {
      return origin + len.value * bboxDim;
    }
    if (len.unit == Lengthd::Unit::Percent) {
      const double refOrigin = isOBB ? (extent == Lengthd::Extent::X ? bboxX : bboxY) : 0.0;
      const double refSize =
          isOBB ? bboxDim : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                                          : primitiveUnitsBounds.height());
      return refOrigin + refSize * len.value / 100.0;
    }
    return len.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  };

  // Resolve a primitive subregion length to user-space size.
  auto resolvePrimitiveSize = [&](const Lengthd& len, Lengthd::Extent extent,
                                  double bboxDim) -> double {
    if (isOBB && len.unit == Lengthd::Unit::None) {
      return len.value * bboxDim;
    }
    if (len.unit == Lengthd::Unit::Percent) {
      const double refSize =
          isOBB ? bboxDim : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                                          : primitiveUnitsBounds.height());
      return refSize * len.value / 100.0;
    }
    return len.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  };

  // Per-node subregion tracking in user space, mirroring tiny-skia's
  // defaultNodeSubregion / resolveInputSubregion / previousOutputSubregion.
  // Used to propagate subregion bounds through the filter graph so that
  // non-source-generator nodes (feOffset, feComposite, etc.) clip to their
  // input bounds rather than the full filter region.
  const Box2d filterRegionSubregion = filterRegion;
  Box2d previousOutputSubregion = filterRegionSubregion;
  std::unordered_map<std::string, Box2d> namedSubregions;

  // Resolve the user-space subregion for a filter input reference.
  auto resolveInputSubregion = [&](const FilterInput& input) -> Box2d {
    if (const auto* named = std::get_if<FilterInput::Named>(&input.value)) {
      auto it = namedSubregions.find(named->name.str());
      if (it != namedSubregions.end()) {
        return it->second;
      }
      return previousOutputSubregion;
    }
    if (std::holds_alternative<FilterInput::Previous>(input.value)) {
      return previousOutputSubregion;
    }
    // StandardInput (SourceGraphic, SourceAlpha, etc.) → full filter region.
    return filterRegionSubregion;
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
      // Project the offset vector through the full CTM so rotation/skew
      // maps correctly to device pixels.
      const Vector2d pixelOffset = toPixelOffset(offset->dx, offset->dy);
      filter_primitive::Offset scaled = *offset;
      scaled.dx = pixelOffset.x;
      scaled.dy = pixelOffset.y;
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
    } else if (const auto* turb = std::get_if<filter_primitive::Turbulence>(&node.primitive)) {
      outputTex = applyTurbulence(inputTex.getWidth(), inputTex.getHeight(), *turb, scaleX, scaleY);
    } else if (const auto* disp =
                   std::get_if<filter_primitive::DisplacementMap>(&node.primitive)) {
      wgpu::Texture in2Tex = inputTex;
      if (node.inputs.size() >= 2) {
        in2Tex = resolveInput(node.inputs[1], namedBuffers, currentBuffer, sourceGraphic);
      }
      // For objectBoundingBox, scale through sqrt(bboxW * bboxH) per the spec.
      double pixelScale = std::abs(disp->scale);
      if (isOBB) {
        pixelScale *= std::sqrt(bboxW * bboxH);
      }
      pixelScale *= std::sqrt(scaleX * scaleY);
      outputTex = applyDisplacementMap(inputTex, in2Tex, *disp, pixelScale);
    } else if (const auto* diffuse =
                   std::get_if<filter_primitive::DiffuseLighting>(&node.primitive)) {
      outputTex = applyDiffuseLighting(inputTex, *diffuse, scaleX, scaleY);
    } else if (const auto* specular =
                   std::get_if<filter_primitive::SpecularLighting>(&node.primitive)) {
      outputTex = applySpecularLighting(inputTex, *specular, scaleX, scaleY);
    } else if (const auto* drop = std::get_if<filter_primitive::DropShadow>(&node.primitive)) {
      // stdDeviation is magnitude only; dx/dy are directional and need
      // the full CTM projection (matching feOffset).
      double sx = drop->stdDeviationX >= 0 ? toPixelX(drop->stdDeviationX) : 0.0;
      double sy = drop->stdDeviationY >= 0 ? toPixelY(drop->stdDeviationY) : 0.0;
      const Vector2d pixelOffset = toPixelOffset(drop->dx, drop->dy);
      outputTex = applyDropShadow(inputTex, *drop, sx, sy, pixelOffset.x, pixelOffset.y);
    } else if (const auto* image = std::get_if<filter_primitive::Image>(&node.primitive)) {
      outputTex =
          applyImage(*image, inputTex.getWidth(), inputTex.getHeight(), graph, node, deviceFromFilter);
    } else if (std::holds_alternative<filter_primitive::Tile>(node.primitive)) {
      // feTile replicates the input's primitive subregion across the filter
      // region. Per SVG §15.28 the source is the primitive's input in its
      // subregion rect; we use the filter region as the default subregion
      // since that's where SourceGraphic content lives in the input texture.
      // Per-primitive subregion overrides on the producing node are not
      // propagated into the FilterGraph yet — a real `in` lookup is follow-
      // up work, matching the existing CPU tile implementation which is
      // similarly best-effort.
      int32_t srcX = 0;
      int32_t srcY = 0;
      int32_t srcW = static_cast<int32_t>(inputTex.getWidth());
      int32_t srcH = static_cast<int32_t>(inputTex.getHeight());
      if (graph.filterRegion.has_value()) {
        const auto& fr = graph.filterRegion.value();
        srcX = static_cast<int32_t>(std::floor(fr.topLeft.x * scaleX));
        srcY = static_cast<int32_t>(std::floor(fr.topLeft.y * scaleY));
        srcW = static_cast<int32_t>(
            std::ceil((fr.bottomRight.x - fr.topLeft.x) * scaleX));
        srcH = static_cast<int32_t>(
            std::ceil((fr.bottomRight.y - fr.topLeft.y) * scaleY));
      }
      outputTex = applyTile(inputTex, srcX, srcY, srcW, srcH);
    } else {
      // Unsupported primitive — pass through unchanged.
      if (verbose_ && !warnedUnsupported_) {
        std::cerr << "GeodeFilterEngine: unsupported filter primitive "
                     "(passthrough)\n";
        warnedUnsupported_ = true;
      }
      outputTex = inputTex;
    }

    // Per-primitive subregion clipping: compute the user-space subregion
    // using input-bounds-based logic matching tiny-skia's defaultNodeSubregion.
    // Source generators (Flood, Turbulence, Image, Tile) and nodes with no
    // inputs default to the filter region.  Other nodes inherit the union of
    // their input subregions, with primitive-specific expansion (e.g. blur).
    // Explicit x/y/width/height attributes override the default and are
    // intersected with the filter region.
    {
      const bool isSourceGenerator =
          std::holds_alternative<filter_primitive::Flood>(node.primitive) ||
          std::holds_alternative<filter_primitive::Turbulence>(node.primitive) ||
          std::holds_alternative<filter_primitive::Image>(node.primitive) ||
          std::holds_alternative<filter_primitive::Tile>(node.primitive);

      const bool hasExplicitSubregion = node.x.has_value() || node.y.has_value() ||
                                       node.width.has_value() || node.height.has_value();

      Box2d nodeSubregion = filterRegionSubregion;

      // Helper: intersect two user-space boxes.
      auto boxIntersect = [](const Box2d& a, const Box2d& b) -> Box2d {
        return Box2d(Vector2d(std::max(a.topLeft.x, b.topLeft.x),
                              std::max(a.topLeft.y, b.topLeft.y)),
                     Vector2d(std::min(a.bottomRight.x, b.bottomRight.x),
                              std::min(a.bottomRight.y, b.bottomRight.y)));
      };

      if (hasExplicitSubregion) {
        // Explicit subregion attributes — resolve and intersect with filter region.
        const double ux =
            node.x.has_value()
                ? resolvePrimitivePosition(*node.x, Lengthd::Extent::X, isOBB ? bboxX : 0.0, bboxW)
                : filterRegion.topLeft.x;
        const double uy =
            node.y.has_value()
                ? resolvePrimitivePosition(*node.y, Lengthd::Extent::Y, isOBB ? bboxY : 0.0, bboxH)
                : filterRegion.topLeft.y;
        const double uw = node.width.has_value()
                              ? resolvePrimitiveSize(*node.width, Lengthd::Extent::X, bboxW)
                              : filterRegion.width();
        const double uh = node.height.has_value()
                              ? resolvePrimitiveSize(*node.height, Lengthd::Extent::Y, bboxH)
                              : filterRegion.height();
        nodeSubregion = boxIntersect(Box2d(Vector2d(ux, uy), Vector2d(ux + uw, uy + uh)),
                                     filterRegionSubregion);
      } else if (!isSourceGenerator && !node.inputs.empty()) {
        // Non-source node without explicit subregion: use union of input bounds.
        Box2d inputBounds = resolveInputSubregion(node.inputs[0]);
        for (size_t i = 1; i < node.inputs.size(); ++i) {
          inputBounds = Box2d::Union(inputBounds, resolveInputSubregion(node.inputs[i]));
        }

        // Apply primitive-specific subregion expansion.
        if (const auto* blur = std::get_if<filter_primitive::GaussianBlur>(&node.primitive)) {
          const double expandX = std::ceil(
              (blur->stdDeviationX >= 0 ? toPixelX(blur->stdDeviationX) : 0.0) * 3.0);
          const double expandY = std::ceil(
              (blur->stdDeviationY >= 0 ? toPixelY(blur->stdDeviationY) : 0.0) * 3.0);
          inputBounds = Box2d(
              Vector2d(inputBounds.topLeft.x - expandX, inputBounds.topLeft.y - expandY),
              Vector2d(inputBounds.bottomRight.x + expandX,
                       inputBounds.bottomRight.y + expandY));
        } else if (const auto* drop =
                       std::get_if<filter_primitive::DropShadow>(&node.primitive)) {
          const double expandX = std::ceil(
              (drop->stdDeviationX >= 0 ? toPixelX(drop->stdDeviationX) : 0.0) * 3.0);
          const double expandY = std::ceil(
              (drop->stdDeviationY >= 0 ? toPixelY(drop->stdDeviationY) : 0.0) * 3.0);
          const Vector2d pixOff = toPixelOffset(drop->dx, drop->dy);
          Box2d shadowBounds = Box2d(
              Vector2d(inputBounds.topLeft.x + pixOff.x - expandX,
                       inputBounds.topLeft.y + pixOff.y - expandY),
              Vector2d(inputBounds.bottomRight.x + pixOff.x + expandX,
                       inputBounds.bottomRight.y + pixOff.y + expandY));
          inputBounds = Box2d::Union(inputBounds, shadowBounds);
        } else if (const auto* morph =
                       std::get_if<filter_primitive::Morphology>(&node.primitive)) {
          if (morph->op == filter_primitive::Morphology::Operator::Dilate) {
            const double rx = toPixelX(morph->radiusX);
            const double ry = toPixelY(morph->radiusY);
            inputBounds = Box2d(
                Vector2d(inputBounds.topLeft.x - rx, inputBounds.topLeft.y - ry),
                Vector2d(inputBounds.bottomRight.x + rx, inputBounds.bottomRight.y + ry));
          }
        }

        nodeSubregion = boxIntersect(inputBounds, filterRegionSubregion);
      }
      // else: source generator or no inputs → nodeSubregion stays as filterRegionSubregion.

      if (hasExplicitSubregion) {
        // Rotation-aware clip using inverse CTM and user-space bounds.
        // Only needed when the node has explicit subregion attributes,
        // matching the CPU path's per-pixel point-in-rect test.
        outputTex = applySubregionClip(outputTex, filterFromDevice,
                                       nodeSubregion.topLeft.x, nodeSubregion.topLeft.y,
                                       nodeSubregion.bottomRight.x, nodeSubregion.bottomRight.y);
      } else {
        // AABB clip in pixel space — project the user-space subregion
        // through the full CTM and clip to the axis-aligned bounding box.
        // This matches the CPU path, which only uses rotation-aware
        // clipping when userSpaceSubregion is set (explicit attributes).
        const Box2d pixelAABB = deviceFromFilter.transformBox(nodeSubregion);
        static const Transform2d kIdentity;
        outputTex = applySubregionClip(outputTex, kIdentity,
                                       pixelAABB.topLeft.x, pixelAABB.topLeft.y,
                                       pixelAABB.bottomRight.x, pixelAABB.bottomRight.y);
      }

      // Record the subregion for downstream nodes.
      if (node.result.has_value()) {
        namedSubregions[node.result->str()] = nodeSubregion;
      }
      previousOutputSubregion = nodeSubregion;
    }

    if (node.result.has_value()) {
      namedBuffers[node.result->str()] = outputTex;
    }

    currentBuffer = outputTex;
  }

  // Clip the final output to the filter region.  The SVG filter model
  // requires pixels outside the filter region to be transparent.
  // Use rotation-aware clipping (via the inverse CTM) so that skewed/rotated
  // filter regions produce correctly-shaped parallelogram clips, matching the
  // CPU path's per-pixel point-in-rect test in ClipFilterOutputToRegion.
  currentBuffer = applySubregionClip(currentBuffer, filterFromDevice, filterRegion.topLeft.x,
                                     filterRegion.topLeft.y, filterRegion.bottomRight.x,
                                     filterRegion.bottomRight.y);

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

  // Morphology is separable: min/max over a 2D rectangle equals
  // row-wise min/max followed by column-wise min/max. Decompose into
  // horizontal (X-only) then vertical (Y-only) passes, each capped at
  // kMaxRadius per pass to stay within the shader's loop limit.
  constexpr int kMaxRadius = 31;
  wgpu::Texture current = input;

  // Horizontal passes (Y radius = 0).
  int remainX = std::max(pixelRadiusX, 0);
  while (remainX > 0) {
    const int passX = std::min(remainX, kMaxRadius);

    wgpu::Texture output =
        createIntermediateTexture(dev, width, height, "FilterMorphologyOutput");

    using Op = svg::components::filter_primitive::Morphology::Operator;
    MorphologyParams params{};
    params.radiusX = passX;
    params.radiusY = 0;
    params.op = primitive.op == Op::Dilate ? 1u : 0u;
    params.pad = 0;

    wgpu::Buffer uniformBuffer =
        createUniformBuffer(device_, &params, sizeof(params), "MorphologyParamsUniform");

    dispatchInputOutputUniform(device_, morphologyBindGroupLayout_, morphologyPipeline_, current,
                               output, uniformBuffer, sizeof(MorphologyParams),
                               "FilterMorphologyPassX");
    current = output;
    remainX -= passX;
  }

  // Vertical passes (X radius = 0).
  int remainY = std::max(pixelRadiusY, 0);
  while (remainY > 0) {
    const int passY = std::min(remainY, kMaxRadius);

    wgpu::Texture output =
        createIntermediateTexture(dev, width, height, "FilterMorphologyOutput");

    using Op = svg::components::filter_primitive::Morphology::Operator;
    MorphologyParams params{};
    params.radiusX = 0;
    params.radiusY = passY;
    params.op = primitive.op == Op::Dilate ? 1u : 0u;
    params.pad = 0;

    wgpu::Buffer uniformBuffer =
        createUniformBuffer(device_, &params, sizeof(params), "MorphologyParamsUniform");

    dispatchInputOutputUniform(device_, morphologyBindGroupLayout_, morphologyPipeline_, current,
                               output, uniformBuffer, sizeof(MorphologyParams),
                               "FilterMorphologyPassY");
    current = output;
    remainY -= passY;
  }

  return current;
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

  const int targetX = primitive.targetX.value_or(primitive.orderX / 2);
  const int targetY = primitive.targetY.value_or(primitive.orderY / 2);
  const int requiredSize = primitive.orderX * primitive.orderY;

  // Compute effective divisor.
  double divisor = 1.0;
  if (primitive.divisor.has_value()) {
    divisor = primitive.divisor.value();
  } else {
    double sum = 0.0;
    for (double v : primitive.kernelMatrix) {
      sum += v;
    }
    divisor = (std::abs(sum) < 1e-10) ? 1.0 : sum;
  }

  // SVG spec validation: invalid parameters produce transparent black.
  // Matches the CPU reference's guard in FilterGraph.cpp.
  // The shader kernel array holds 25 elements, so any orderX*orderY <= 25 is valid.
  const bool invalid = primitive.orderX <= 0 || primitive.orderY <= 0 ||
                        primitive.orderX * primitive.orderY > 25 ||
                        static_cast<int>(primitive.kernelMatrix.size()) != requiredSize ||
                        targetX < 0 || targetX >= primitive.orderX ||
                        targetY < 0 || targetY >= primitive.orderY ||
                        (primitive.divisor.has_value() && primitive.divisor.value() == 0.0);

  if (invalid) {
    if (verbose_) {
      std::cerr << "GeodeFilterEngine: feConvolveMatrix invalid params ("
                << primitive.orderX << "×" << primitive.orderY
                << ", targetX=" << targetX << ", targetY=" << targetY
                << ", divisor=" << divisor
                << "); outputting transparent black\n";
    }
    // Return a transparent texture (matches CPU reference behavior).
    return createIntermediateTexture(dev, width, height, "FilterConvolveMatrixTransparent");
  }

  wgpu::Texture output =
      createIntermediateTexture(dev, width, height, "FilterConvolveMatrixOutput");

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

wgpu::Texture GeodeFilterEngine::applyTurbulence(
    uint32_t width, uint32_t height,
    const svg::components::filter_primitive::Turbulence& primitive, double scaleX, double scaleY) {
  const wgpu::Device& dev = device_.device();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterTurbulenceOutput");

  // Negative baseFrequency is invalid per SVG Filter Effects §15.20.3; produce transparent black
  // (matching resvg/tiny-skia behavior).
  if (primitive.baseFrequencyX < 0.0 || primitive.baseFrequencyY < 0.0) {
    return output;
  }

  TurbulenceParams params{};
  params.baseFreqX = static_cast<float>(primitive.baseFrequencyX);
  params.baseFreqY = static_cast<float>(primitive.baseFrequencyY);
  params.numOctaves = primitive.numOctaves;
  params.seed = static_cast<int32_t>(primitive.seed);
  params.stitchTiles = primitive.stitchTiles ? 1u : 0u;
  params.typeFlag =
      primitive.type == svg::components::filter_primitive::Turbulence::Type::Turbulence ? 1u : 0u;
  // Tile dimensions in user space (for stitchTiles).
  params.tileWidth = static_cast<float>(width / scaleX);
  params.tileHeight = static_cast<float>(height / scaleY);
  // Inverse device-to-filter transform: for now assume identity (no skew/rotation).
  params.filterFromDeviceA = static_cast<float>(1.0 / scaleX);
  params.filterFromDeviceB = 0.0f;
  params.filterFromDeviceC = 0.0f;
  params.filterFromDeviceD = static_cast<float>(1.0 / scaleY);

  // Generate permutation + gradient tables from the seed.
  TurbulenceTables tables{};
  generateTurbulenceTables(primitive.seed, tables);

  // Upload params as storage buffer.
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("TurbulenceParamsStorage");
  bufDesc.size = sizeof(TurbulenceParams);
  bufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer paramsBuffer = dev.createBuffer(bufDesc);
  device_.queue().writeBuffer(paramsBuffer, 0, &params, sizeof(params));

  // Upload tables as storage buffer.
  wgpu::BufferDescriptor tablesBufDesc{};
  tablesBufDesc.label = wgpuLabel("TurbulenceTablesStorage");
  tablesBufDesc.size = sizeof(TurbulenceTables);
  tablesBufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  tablesBufDesc.mappedAtCreation = false;
  wgpu::Buffer tablesBuffer = dev.createBuffer(tablesBufDesc);
  device_.queue().writeBuffer(tablesBuffer, 0, &tables, sizeof(tables));

  wgpu::TextureView outputView = output.createView();

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = outputView;
  bgEntries[1].binding = 1;
  bgEntries[1].buffer = paramsBuffer;
  bgEntries[1].offset = 0;
  bgEntries[1].size = sizeof(TurbulenceParams);
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = tablesBuffer;
  bgEntries[2].offset = 0;
  bgEntries[2].size = sizeof(TurbulenceTables);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterTurbulenceBindGroup");
  bgDesc.layout = turbulenceBindGroupLayout_;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel("FilterTurbulenceEncoder");
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterTurbulencePass");
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(turbulencePipeline_);
  pass.setBindGroup(0, bindGroup, 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.end();

  wgpu::CommandBuffer cmdBuf = encoder.finish();
  device_.queue().submit(1, &cmdBuf);
  return output;
}

wgpu::Texture GeodeFilterEngine::applyDisplacementMap(
    const wgpu::Texture& in1, const wgpu::Texture& in2,
    const svg::components::filter_primitive::DisplacementMap& primitive, double pixelScale) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = in1.getWidth();
  const uint32_t height = in1.getHeight();

  wgpu::Texture output =
      createIntermediateTexture(dev, width, height, "FilterDisplacementMapOutput");

  using Channel = svg::components::filter_primitive::DisplacementMap::Channel;
  auto toIndex = [](Channel ch) -> uint32_t {
    switch (ch) {
      case Channel::R: return 0;
      case Channel::G: return 1;
      case Channel::B: return 2;
      case Channel::A: return 3;
    }
    return 3;
  };

  DisplacementParams params{};
  params.scale = static_cast<float>(pixelScale);
  params.xChannel = toIndex(primitive.xChannelSelector);
  params.yChannel = toIndex(primitive.yChannelSelector);
  params.pad = 0;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "DisplacementMapParamsUniform");

  dispatchTwoInputUniform(device_, displacementMapBindGroupLayout_, displacementMapPipeline_, in1,
                          in2, output, uniformBuffer, sizeof(DisplacementParams),
                          "FilterDisplacementMapPass");
  return output;
}

namespace {

/// Fill common light-source fields into the params struct fields starting at
/// the given pointers. This avoids duplicating the logic between diffuse and specular.
void fillLightParams(const svg::components::filter_primitive::LightSource& light, double scaleX,
                     double scaleY, uint32_t* lightType, float* azimuthRad, float* elevationRad,
                     float* lightX, float* lightY, float* lightZ, float* pointsAtX,
                     float* pointsAtY, float* pointsAtZ, float* spotExponent,
                     float* cosConeAngle) {
  using LT = svg::components::filter_primitive::LightSource::Type;
  switch (light.type) {
    case LT::Distant: *lightType = 0; break;
    case LT::Point: *lightType = 1; break;
    case LT::Spot: *lightType = 2; break;
  }

  constexpr double kDegToRad = std::numbers::pi / 180.0;
  *azimuthRad = static_cast<float>(light.azimuth * kDegToRad);
  *elevationRad = static_cast<float>(light.elevation * kDegToRad);

  // Point/Spot positions are in user space — convert to pixel space.
  *lightX = static_cast<float>(light.x * scaleX);
  *lightY = static_cast<float>(light.y * scaleY);
  *lightZ = static_cast<float>(light.z * std::sqrt(scaleX * scaleY));
  *pointsAtX = static_cast<float>(light.pointsAtX * scaleX);
  *pointsAtY = static_cast<float>(light.pointsAtY * scaleY);
  *pointsAtZ = static_cast<float>(light.pointsAtZ * std::sqrt(scaleX * scaleY));
  *spotExponent = static_cast<float>(light.spotExponent);
  *cosConeAngle = light.limitingConeAngle.has_value()
                      ? static_cast<float>(std::cos(light.limitingConeAngle.value() * kDegToRad))
                      : -2.0f;
}

}  // namespace

wgpu::Texture GeodeFilterEngine::applyDiffuseLighting(
    const wgpu::Texture& input,
    const svg::components::filter_primitive::DiffuseLighting& primitive, double scaleX,
    double scaleY) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  wgpu::Texture output =
      createIntermediateTexture(dev, width, height, "FilterDiffuseLightingOutput");

  DiffuseLightingParams params{};
  params.surfaceScale = static_cast<float>(primitive.surfaceScale);
  params.diffuseConstant = static_cast<float>(primitive.diffuseConstant);
  params.pad0 = 0.0f;
  params.pad1 = 0.0f;

  const css::RGBA rgba = primitive.lightingColor.asRGBA();
  params.lightR = static_cast<float>(rgba.r) / 255.0f;
  params.lightG = static_cast<float>(rgba.g) / 255.0f;
  params.lightB = static_cast<float>(rgba.b) / 255.0f;

  if (primitive.light.has_value()) {
    fillLightParams(primitive.light.value(), scaleX, scaleY, &params.lightType, &params.azimuthRad,
                    &params.elevationRad, &params.lightX, &params.lightY, &params.lightZ,
                    &params.pointsAtX, &params.pointsAtY, &params.pointsAtZ, &params.spotExponent,
                    &params.cosConeAngle);
  } else {
    // Default: distant light with azimuth=0, elevation=0.
    params.lightType = 0;
    params.azimuthRad = 0.0f;
    params.elevationRad = 0.0f;
  }

  // Upload as storage buffer.
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("DiffuseLightingParamsStorage");
  bufDesc.size = sizeof(DiffuseLightingParams);
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
  bgEntries[2].size = sizeof(DiffuseLightingParams);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterDiffuseLightingBindGroup");
  bgDesc.layout = diffuseLightingBindGroupLayout_;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel("FilterDiffuseLightingEncoder");
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterDiffuseLightingPass");
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(diffuseLightingPipeline_);
  pass.setBindGroup(0, bindGroup, 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.end();

  wgpu::CommandBuffer cmdBuf = encoder.finish();
  device_.queue().submit(1, &cmdBuf);
  return output;
}

wgpu::Texture GeodeFilterEngine::applySpecularLighting(
    const wgpu::Texture& input,
    const svg::components::filter_primitive::SpecularLighting& primitive, double scaleX,
    double scaleY) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  wgpu::Texture output =
      createIntermediateTexture(dev, width, height, "FilterSpecularLightingOutput");

  SpecularLightingParams params{};
  params.surfaceScale = static_cast<float>(primitive.surfaceScale);
  params.specularConstant = static_cast<float>(primitive.specularConstant);
  params.specularExponent = static_cast<float>(std::clamp(primitive.specularExponent, 1.0, 128.0));
  params.pad0 = 0.0f;

  const css::RGBA rgba = primitive.lightingColor.asRGBA();
  params.lightR = static_cast<float>(rgba.r) / 255.0f;
  params.lightG = static_cast<float>(rgba.g) / 255.0f;
  params.lightB = static_cast<float>(rgba.b) / 255.0f;

  if (primitive.light.has_value()) {
    fillLightParams(primitive.light.value(), scaleX, scaleY, &params.lightType, &params.azimuthRad,
                    &params.elevationRad, &params.lightX, &params.lightY, &params.lightZ,
                    &params.pointsAtX, &params.pointsAtY, &params.pointsAtZ, &params.spotExponent,
                    &params.cosConeAngle);
  } else {
    params.lightType = 0;
    params.azimuthRad = 0.0f;
    params.elevationRad = 0.0f;
  }

  // Upload as storage buffer.
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("SpecularLightingParamsStorage");
  bufDesc.size = sizeof(SpecularLightingParams);
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
  bgEntries[2].size = sizeof(SpecularLightingParams);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterSpecularLightingBindGroup");
  bgDesc.layout = specularLightingBindGroupLayout_;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  wgpu::BindGroup bindGroup = dev.createBindGroup(bgDesc);

  wgpu::CommandEncoderDescriptor ceDesc{};
  ceDesc.label = wgpuLabel("FilterSpecularLightingEncoder");
  wgpu::CommandEncoder encoder = dev.createCommandEncoder(ceDesc);

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterSpecularLightingPass");
  wgpu::ComputePassEncoder pass = encoder.beginComputePass(passDesc);
  pass.setPipeline(specularLightingPipeline_);
  pass.setBindGroup(0, bindGroup, 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.end();

  wgpu::CommandBuffer cmdBuf = encoder.finish();
  device_.queue().submit(1, &cmdBuf);
  return output;
}

wgpu::Texture GeodeFilterEngine::applyDropShadow(
    const wgpu::Texture& input,
    const svg::components::filter_primitive::DropShadow& primitive, double pixelStdDevX,
    double pixelStdDevY, double pixelDx, double pixelDy) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  // Blur the input first (if any deviation). The compose shader reads the
  // blurred texture's alpha at (coord - offset); blurring RGB at the same
  // time is free because we already have to walk the taps.
  wgpu::Texture blurred = applyGaussianBlur(input, pixelStdDevX, pixelStdDevY, /*edgeMode=*/0);

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterDropShadowOutput");

  const css::RGBA rgba = primitive.floodColor.asRGBA();
  const float floodA = (static_cast<float>(rgba.a) / 255.0f) *
                       static_cast<float>(std::clamp(primitive.floodOpacity, 0.0, 1.0));

  DropShadowParams params{};
  // Straight-alpha flood color; the shader premultiplies by the blurred
  // source's alpha per-pixel rather than baking it in here.
  params.color[0] = static_cast<float>(rgba.r) / 255.0f;
  params.color[1] = static_cast<float>(rgba.g) / 255.0f;
  params.color[2] = static_cast<float>(rgba.b) / 255.0f;
  params.color[3] = floodA;
  params.dx = static_cast<float>(pixelDx);
  params.dy = static_cast<float>(pixelDy);
  params.pad0 = 0;
  params.pad1 = 0;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "DropShadowParamsUniform");

  dispatchTwoInputUniform(device_, dropShadowBindGroupLayout_, dropShadowPipeline_, input, blurred,
                          output, uniformBuffer, sizeof(DropShadowParams), "FilterDropShadowPass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyImage(
    const svg::components::filter_primitive::Image& primitive, uint32_t width, uint32_t height,
    const svg::components::FilterGraph& graph, const svg::components::FilterNode& node,
    const Transform2d& deviceFromFilter) {
  const wgpu::Device& dev = device_.device();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterImageOutput");

  // Empty / degenerate source: feed the shader a 1×1 transparent texture
  // with an out-of-bounds transform. The resulting output is transparent
  // everywhere — which is also what the shader would emit for any real
  // image whose sample coordinates fall outside the source bounds.
  if (primitive.imageData.empty() || primitive.imageWidth <= 0 || primitive.imageHeight <= 0) {
    wgpu::TextureDescriptor imgDesc{};
    imgDesc.label = wgpuLabel("FilterImageEmptySource");
    imgDesc.size = {1, 1, 1};
    imgDesc.format = kFormat;
    imgDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    imgDesc.mipLevelCount = 1;
    imgDesc.sampleCount = 1;
    imgDesc.dimension = wgpu::TextureDimension::_2D;
    wgpu::Texture emptyTex = dev.createTexture(imgDesc);
    const uint8_t zero[4] = {0, 0, 0, 0};
    wgpu::TexelCopyTextureInfo dstInfo{};
    dstInfo.texture = emptyTex;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    wgpu::Extent3D extent = {1, 1, 1};
    device_.queue().writeTexture(dstInfo, zero, 4, layout, extent);

    ImageParams params{};
    params.m02 = -1000.0f;
    params.m12 = -1000.0f;
    wgpu::Buffer ub = createUniformBuffer(device_, &params, sizeof(params), "ImageParamsEmpty");
    dispatchInputOutputUniform(device_, imageBindGroupLayout_, imagePipeline_, emptyTex, output, ub,
                               sizeof(ImageParams), "FilterImageEmptyPass");
    return output;
  }

  // Upload the image's straight-alpha RGBA pixels as a premultiplied
  // texture — Geode operates in premultiplied throughout the filter graph
  // (consistent with feFlood / feMerge).
  const int imgW = primitive.imageWidth;
  const int imgH = primitive.imageHeight;
  std::vector<uint8_t> premul(primitive.imageData.size());
  for (size_t i = 0; i + 3 < primitive.imageData.size(); i += 4) {
    const unsigned a = primitive.imageData[i + 3];
    premul[i + 0] =
        static_cast<uint8_t>((static_cast<unsigned>(primitive.imageData[i + 0]) * a + 127u) / 255u);
    premul[i + 1] =
        static_cast<uint8_t>((static_cast<unsigned>(primitive.imageData[i + 1]) * a + 127u) / 255u);
    premul[i + 2] =
        static_cast<uint8_t>((static_cast<unsigned>(primitive.imageData[i + 2]) * a + 127u) / 255u);
    premul[i + 3] = static_cast<uint8_t>(a);
  }

  wgpu::TextureDescriptor imgDesc{};
  imgDesc.label = wgpuLabel("FilterImageSource");
  imgDesc.size = {static_cast<uint32_t>(imgW), static_cast<uint32_t>(imgH), 1};
  imgDesc.format = kFormat;
  imgDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  imgDesc.mipLevelCount = 1;
  imgDesc.sampleCount = 1;
  imgDesc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture imgTex = dev.createTexture(imgDesc);

  wgpu::TexelCopyTextureInfo dstInfo{};
  dstInfo.texture = imgTex;
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = static_cast<uint32_t>(imgW * 4);
  layout.rowsPerImage = static_cast<uint32_t>(imgH);
  wgpu::Extent3D extent = {static_cast<uint32_t>(imgW), static_cast<uint32_t>(imgH), 1};
  device_.queue().writeTexture(dstInfo, premul.data(), premul.size(), layout, extent);

  // Fragment references with a non-axis-aligned ancestor transform: project the fragment image
  // through the full CTM (rotation/skew) so placement matches the CPU path. The transform
  // chain is:
  //   fragment pixel → (÷ viewBoxScale) → document user space
  //   → (+ filterRegion.topLeft) → host user space
  //   → (× deviceFromFilter) → device pixels
  // The shader receives the inverse: device pixel → fragment pixel.
  const bool hasRotation = !NearZero(deviceFromFilter.data[1], 1e-6) ||
                           !NearZero(deviceFromFilter.data[2], 1e-6);
  if (primitive.isFragmentReference && hasRotation &&
      !NearZero(deviceFromFilter.determinant(), 1e-12)) {
    const double uScaleX = graph.userToPixelScale.x;
    const double uScaleY = graph.userToPixelScale.y;
    const Transform2d viewBoxScaleInv = Transform2d::Scale(
        NearZero(uScaleX, 1e-12) ? 1.0 : 1.0 / uScaleX,
        NearZero(uScaleY, 1e-12) ? 1.0 : 1.0 / uScaleY);
    const Transform2d regionOffset = Transform2d::Translate(
        primitive.fragmentRegionTopLeft.x, primitive.fragmentRegionTopLeft.y);
    const Transform2d deviceFromFragment = viewBoxScaleInv * regionOffset * deviceFromFilter;
    const Transform2d fragmentFromDevice = deviceFromFragment.inverse();

    ImageParams params{};
    params.m00 = static_cast<float>(fragmentFromDevice.data[0]);
    params.m01 = static_cast<float>(fragmentFromDevice.data[2]);
    params.m02 = static_cast<float>(fragmentFromDevice.data[4]);
    params.m10 = static_cast<float>(fragmentFromDevice.data[1]);
    params.m11 = static_cast<float>(fragmentFromDevice.data[3]);
    params.m12 = static_cast<float>(fragmentFromDevice.data[5]);
    params.pad0 = 0;
    params.pad1 = 0;

    wgpu::Buffer uniformBuffer =
        createUniformBuffer(device_, &params, sizeof(params), "ImageParamsFragRef");
    dispatchInputOutputUniform(device_, imageBindGroupLayout_, imagePipeline_, imgTex, output,
                               uniformBuffer, sizeof(ImageParams), "FilterImageFragRefPass");
    return output;
  }

  // Fragment references without rotation: simple device-space post-translation to position
  // content at the filter region origin, matching the CPU path's non-rotated fragment case.
  if (primitive.isFragmentReference) {
    const double uScaleX = graph.userToPixelScale.x > 0.0 ? graph.userToPixelScale.x : 1.0;
    const double uScaleY = graph.userToPixelScale.y > 0.0 ? graph.userToPixelScale.y : 1.0;
    const double deviceOffsetX = primitive.fragmentRegionTopLeft.x * uScaleX;
    const double deviceOffsetY = primitive.fragmentRegionTopLeft.y * uScaleY;

    ImageParams params{};
    params.m00 = 1.0f;
    params.m01 = 0.0f;
    params.m02 = static_cast<float>(-deviceOffsetX);
    params.m10 = 0.0f;
    params.m11 = 1.0f;
    params.m12 = static_cast<float>(-deviceOffsetY);
    params.pad0 = 0;
    params.pad1 = 0;

    wgpu::Buffer uniformBuffer =
        createUniformBuffer(device_, &params, sizeof(params), "ImageParamsFragRef");
    dispatchInputOutputUniform(device_, imageBindGroupLayout_, imagePipeline_, imgTex, output,
                               uniformBuffer, sizeof(ImageParams), "FilterImageFragRefPass");
    return output;
  }

  // Work out the placement rectangle in output-pixel coordinates. Prefer
  // the node's primitive subregion when given; fall back to the filter
  // region so the image covers the full primitive area.
  const double scaleX = graph.userToPixelScale.x > 0.0 ? graph.userToPixelScale.x : 1.0;
  const double scaleY = graph.userToPixelScale.y > 0.0 ? graph.userToPixelScale.y : 1.0;
  const bool isOBB = graph.primitiveUnits == svg::PrimitiveUnits::ObjectBoundingBox;
  const double bboxW = graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->width()
                                                             : 1.0;
  const double bboxH = graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->height()
                                                             : 1.0;

  double regionX = 0.0;
  double regionY = 0.0;
  double regionW = static_cast<double>(width);
  double regionH = static_cast<double>(height);
  if (node.x.has_value() && node.y.has_value() && node.width.has_value() && node.height.has_value()) {
    const double rx = isOBB ? node.x->value * bboxW : node.x->value;
    const double ry = isOBB ? node.y->value * bboxH : node.y->value;
    const double rw = isOBB ? node.width->value * bboxW : node.width->value;
    const double rh = isOBB ? node.height->value * bboxH : node.height->value;
    regionX = rx * scaleX;
    regionY = ry * scaleY;
    regionW = rw * scaleX;
    regionH = rh * scaleY;
  } else if (graph.filterRegion.has_value()) {
    const auto& fr = graph.filterRegion.value();
    regionX = fr.topLeft.x * scaleX;
    regionY = fr.topLeft.y * scaleY;
    regionW = (fr.bottomRight.x - fr.topLeft.x) * scaleX;
    regionH = (fr.bottomRight.y - fr.topLeft.y) * scaleY;
  }

  if (regionW <= 0.0 || regionH <= 0.0) {
    regionW = static_cast<double>(imgW);
    regionH = static_cast<double>(imgH);
  }

  // Compute the src-from-dst transform (image-from-output). For a
  // straight image placement at (regionX, regionY) with size (regionW,
  // regionH) and source size (imgW, imgH), the transform is:
  //   src.x = (dst.x - regionX) * imgW / regionW
  //   src.y = (dst.y - regionY) * imgH / regionH
  //
  // preserveAspectRatio: meet / slice scales isotropically; 'none'
  // scales independently as above. The simple non-preserveAR case covers
  // the common resvg test cases — the preserveAspectRatio computation
  // mirrors the CPU feImage path's convention (fit box to subregion).
  double scaleImgX = regionW > 0.0 ? static_cast<double>(imgW) / regionW : 1.0;
  double scaleImgY = regionH > 0.0 ? static_cast<double>(imgH) / regionH : 1.0;
  double offsetX = -regionX * scaleImgX;
  double offsetY = -regionY * scaleImgY;

  using PAR = svg::PreserveAspectRatio;
  if (primitive.preserveAspectRatio.align != PAR::Align::None) {
    // Uniform scale per preserveAspectRatio semantics. Compute the
    // meet/slice scale and the alignment offsets, then invert to build
    // image-from-output.
    const double scaleMeet = std::min(regionW / static_cast<double>(imgW),
                                       regionH / static_cast<double>(imgH));
    const double scaleSlice = std::max(regionW / static_cast<double>(imgW),
                                        regionH / static_cast<double>(imgH));
    const double s = primitive.preserveAspectRatio.meetOrSlice == PAR::MeetOrSlice::Slice
                         ? scaleSlice
                         : scaleMeet;
    const double drawnW = static_cast<double>(imgW) * s;
    const double drawnH = static_cast<double>(imgH) * s;

    // Alignment within the subregion.
    using Align = PAR::Align;
    double alignX = 0.0;
    double alignY = 0.0;
    switch (primitive.preserveAspectRatio.align) {
      case Align::None:
      case Align::XMinYMin: alignX = 0.0; alignY = 0.0; break;
      case Align::XMidYMin: alignX = 0.5; alignY = 0.0; break;
      case Align::XMaxYMin: alignX = 1.0; alignY = 0.0; break;
      case Align::XMinYMid: alignX = 0.0; alignY = 0.5; break;
      case Align::XMidYMid: alignX = 0.5; alignY = 0.5; break;
      case Align::XMaxYMid: alignX = 1.0; alignY = 0.5; break;
      case Align::XMinYMax: alignX = 0.0; alignY = 1.0; break;
      case Align::XMidYMax: alignX = 0.5; alignY = 1.0; break;
      case Align::XMaxYMax: alignX = 1.0; alignY = 1.0; break;
    }
    const double drawX = regionX + (regionW - drawnW) * alignX;
    const double drawY = regionY + (regionH - drawnH) * alignY;

    scaleImgX = 1.0 / s;
    scaleImgY = 1.0 / s;
    offsetX = -drawX / s;
    offsetY = -drawY / s;
  }

  ImageParams params{};
  params.m00 = static_cast<float>(scaleImgX);
  params.m01 = 0.0f;
  params.m02 = static_cast<float>(offsetX);
  params.m10 = 0.0f;
  params.m11 = static_cast<float>(scaleImgY);
  params.m12 = static_cast<float>(offsetY);
  params.pad0 = 0;
  params.pad1 = 0;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "ImageParamsUniform");

  dispatchInputOutputUniform(device_, imageBindGroupLayout_, imagePipeline_, imgTex, output,
                             uniformBuffer, sizeof(ImageParams), "FilterImagePass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyTile(const wgpu::Texture& input, int32_t srcX, int32_t srcY,
                                            int32_t srcW, int32_t srcH) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterTileOutput");

  TileParams params{};
  params.srcX = srcX;
  params.srcY = srcY;
  params.srcW = srcW;
  params.srcH = srcH;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "TileParamsUniform");

  dispatchInputOutputUniform(device_, tileBindGroupLayout_, tilePipeline_, input, output,
                             uniformBuffer, sizeof(TileParams), "FilterTilePass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applySubregionClip(const wgpu::Texture& input,
                                                    const Transform2d& filterFromDevice,
                                                    double usrX0, double usrY0, double usrX1,
                                                    double usrY1) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  wgpu::Texture output = createIntermediateTexture(dev, width, height, "FilterSubregionClipOutput");

  SubregionClipParams params{};
  params.invA = static_cast<float>(filterFromDevice.data[0]);
  params.invB = static_cast<float>(filterFromDevice.data[1]);
  params.invC = static_cast<float>(filterFromDevice.data[2]);
  params.invD = static_cast<float>(filterFromDevice.data[3]);
  params.invE = static_cast<float>(filterFromDevice.data[4]);
  params.invF = static_cast<float>(filterFromDevice.data[5]);
  params.usrX0 = static_cast<float>(usrX0);
  params.usrY0 = static_cast<float>(usrY0);
  params.usrX1 = static_cast<float>(usrX1);
  params.usrY1 = static_cast<float>(usrY1);
  params.pad0 = 0;
  params.pad1 = 0;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "SubregionClipParamsUniform");

  dispatchInputOutputUniform(device_, subregionClipBindGroupLayout_, subregionClipPipeline_, input,
                             output, uniformBuffer, sizeof(SubregionClipParams),
                             "FilterSubregionClipPass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyColorSpaceConversion(const wgpu::Texture& input,
                                                           bool srgbToLinear) {
  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  wgpu::Texture output =
      createIntermediateTexture(dev, width, height, "FilterColorSpaceConvertOutput");

  ColorSpaceConvertParams params{};
  params.direction = srgbToLinear ? 0u : 1u;

  wgpu::Buffer uniformBuffer =
      createUniformBuffer(device_, &params, sizeof(params), "ColorSpaceConvertParamsUniform");

  dispatchInputOutputUniform(device_, colorSpaceConvertBindGroupLayout_,
                             colorSpaceConvertPipeline_, input, output, uniformBuffer,
                             sizeof(ColorSpaceConvertParams), "FilterColorSpaceConvertPass");
  return output;
}

}  // namespace donner::geode
