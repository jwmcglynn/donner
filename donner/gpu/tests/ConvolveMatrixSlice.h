#pragma once
/// @file
/// Native matrix-convolution acceptance with an independently evaluated reference image.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/ConvolveMatrixBindings.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace donner::gpu::tests {
namespace convolve_matrix_slice {

inline constexpr uint32_t kWidth = 5;
inline constexpr uint32_t kHeight = 4;
inline constexpr uint32_t kBytesPerRow = 256;

using Params = shader::programs::ConvolveMatrixParams;

/// Varied premultiplied source whose position, channels, and fractional alpha are independent.
inline std::array<float, kWidth * kHeight * 4> InputTexels() {
  std::array<float, kWidth * kHeight * 4> result{};
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const size_t offset = size_t{y * kWidth + x} * 4;
      const float alpha = 0.2f + 0.1f * static_cast<float>((x + 2 * y) % 7);
      result[offset] = alpha * static_cast<float>(x + 1) / 7.0f;
      result[offset + 1] = alpha * static_cast<float>(y + 1) / 6.0f;
      result[offset + 2] = alpha * static_cast<float>((3 * x + 5 * y) % 11 + 1) / 13.0f;
      result[offset + 3] = alpha;
    }
  }
  return result;
}

/// Asymmetric kernel and off-center target expose transposition, reversal, and offset errors.
inline Params Parameters(uint32_t edgeMode, bool preserveAlpha) {
  Params result{2, 3, 0, 1, 2.0f, 0.04f, edgeMode, preserveAlpha ? 1u : 0u, {}};
  const std::array<float, 6> coefficients{0.25f, -0.5f, 1.25f, 0.75f, -0.25f, 0.5f};
  std::copy(coefficients.begin(), coefficients.end(), result.kernel);
  return result;
}

inline int32_t Wrap(int32_t value, int32_t extent) {
  return (value % extent + extent) % extent;
}

/// Independently evaluates one convolution output texel from the input and host parameters.
inline std::array<float, 4> ExpectedTexel(const std::array<float, kWidth * kHeight * 4>& input,
                                          const Params& params, int32_t x, int32_t y) {
  std::array<float, 3> rgb{};
  float alpha = 0.0f;
  for (int32_t kernelY = 0; kernelY < params.orderY; ++kernelY) {
    for (int32_t kernelX = 0; kernelX < params.orderX; ++kernelX) {
      int32_t sourceX = x + kernelX - params.targetX;
      int32_t sourceY = y + kernelY - params.targetY;
      if (params.edgeMode == 0) {
        sourceX = std::clamp(sourceX, 0, static_cast<int32_t>(kWidth) - 1);
        sourceY = std::clamp(sourceY, 0, static_cast<int32_t>(kHeight) - 1);
      } else if (params.edgeMode == 1) {
        sourceX = Wrap(sourceX, kWidth);
        sourceY = Wrap(sourceY, kHeight);
      } else if (sourceX < 0 || sourceY < 0 || sourceX >= static_cast<int32_t>(kWidth) ||
                 sourceY >= static_cast<int32_t>(kHeight)) {
        continue;
      }
      const size_t sourceOffset = size_t{sourceY * kWidth + sourceX} * 4;
      const int32_t coefficientIndex =
          (params.orderY - 1 - kernelY) * params.orderX + params.orderX - 1 - kernelX;
      const float coefficient = params.kernel[coefficientIndex];
      const float sourceAlpha = input[sourceOffset + 3];
      for (size_t channel = 0; channel < 3; ++channel) {
        const float color = params.preserveAlpha != 0 && sourceAlpha > 0.0f
                                ? input[sourceOffset + channel] / sourceAlpha
                                : input[sourceOffset + channel];
        rgb[channel] += color * coefficient;
      }
      alpha += sourceAlpha * coefficient;
    }
  }

  const float sourceAlpha = input[size_t{y * kWidth + x} * 4 + 3];
  if (params.preserveAlpha != 0) {
    for (float& channel : rgb) {
      channel = std::clamp(channel / params.divisor + params.bias, 0.0f, 1.0f) * sourceAlpha;
    }
    return {rgb[0], rgb[1], rgb[2], sourceAlpha};
  }

  const float scaledBias = params.bias * sourceAlpha;
  alpha = std::clamp(alpha / params.divisor + scaledBias, 0.0f, 1.0f);
  for (float& channel : rgb) {
    channel = std::clamp(channel / params.divisor + scaledBias, 0.0f, alpha);
  }
  return {rgb[0], rgb[1], rgb[2], alpha};
}

}  // namespace convolve_matrix_slice

/**
 * Runs a nonuniform 2x3 convolution at a non-workgroup-aligned extent and compares it to an
 * independently evaluated reference through the repository's strict pixelmatch path.
 *
 * @param device Native device with bounded wait/readback support.
 * @param shaderDescriptor Backend-generated module descriptor.
 * @param readbackBuffer Reads a submitted buffer through the backend's host mapping API.
 * @param edgeMode Zero for duplicate, one for wrap, and two for transparent black.
 * @param preserveAlpha Whether to convolve straight RGB while retaining source alpha.
 */
template <typename DeviceType, typename Readback>
void CheckConvolveMatrixStorage(DeviceType& device, const ShaderModuleDescriptor& shaderDescriptor,
                                Readback readbackBuffer, uint32_t edgeMode, bool preserveAlpha) {
  using namespace convolve_matrix_slice;
  using shader::programs::ConvolveMatrixBinding;
  const auto binding = [](ConvolveMatrixBinding value) { return static_cast<uint32_t>(value); };

  Result<ShaderModule> shaderModule = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shaderModule, HasResult());
  Result<BindGroupLayout> layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "convolve matrix",
      {{binding(ConvolveMatrixBinding::InputTexture), ShaderStage::Compute,
        BindingType::SampledTexture2dUnfilterableFloat},
       {binding(ConvolveMatrixBinding::OutputTexture), ShaderStage::Compute,
        BindingType::WriteOnlyStorageTexture2d, TextureFormat::RGBA32Float},
       {binding(ConvolveMatrixBinding::Params), ShaderStage::Compute,
        BindingType::ReadOnlyStorageBuffer}}});
  ASSERT_THAT(layout, HasResult());
  Result<PipelineLayout> pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"convolve matrix", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  Result<ComputePipeline> pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "convolve matrix",
      pipelineLayout.result(),
      ComputeState{shaderModule.result(), RcString(shader::programs::kConvolveMatrixEntryPoint)},
      {shader::programs::kConvolveMatrixWorkgroupSize,
       shader::programs::kConvolveMatrixWorkgroupSize, 1}});
  ASSERT_THAT(pipeline, HasResult());

  Result<Texture> input =
      device.createTexture(TextureDescriptor{"convolve input",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  Result<Texture> output =
      device.createTexture(TextureDescriptor{"convolve output",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(input, HasResult());
  ASSERT_THAT(output, HasResult());
  Result<TextureView> inputView = device.createTextureView(input.result(), {"convolve input"});
  Result<TextureView> outputView = device.createTextureView(output.result(), {"convolve output"});
  ASSERT_THAT(inputView, HasResult());
  ASSERT_THAT(outputView, HasResult());

  const std::array<float, kWidth * kHeight * 4> inputTexels = InputTexels();
  std::array<uint8_t, kBytesPerRow * kHeight> upload{};
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(upload.data() + size_t{y} * kBytesPerRow,
                inputTexels.data() + size_t{y} * kWidth * 4, kWidth * 4 * sizeof(float));
  }
  ASSERT_THAT(
      device.writeTexture(input.result(), upload, {0, kBytesPerRow, kHeight}, {kWidth, kHeight}),
      IsOk());

  const Params params = Parameters(edgeMode, preserveAlpha);
  Result<Buffer> paramsBuffer = device.createBuffer(BufferDescriptor{
      "convolve parameters", sizeof(params), BufferUsage::Storage | BufferUsage::CopyDst});
  ASSERT_THAT(paramsBuffer, HasResult());
  ASSERT_THAT(device.writeBuffer(paramsBuffer.result(), 0,
                                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&params),
                                                          sizeof(params))),
              IsOk());
  Result<BindGroup> bindGroup = device.createBindGroup(BindGroupDescriptor{
      "convolve matrix",
      layout.result(),
      {{binding(ConvolveMatrixBinding::InputTexture), TextureViewBinding{inputView.result()}},
       {binding(ConvolveMatrixBinding::OutputTexture), TextureViewBinding{outputView.result()}},
       {binding(ConvolveMatrixBinding::Params),
        BufferBinding{paramsBuffer.result(), 0, sizeof(params)}}}});
  ASSERT_THAT(bindGroup, HasResult());

  Result<Buffer> readback =
      device.createBuffer(BufferDescriptor{"convolve readback", uint64_t{kBytesPerRow} * kHeight,
                                           BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  Result<std::unique_ptr<CommandEncoder>> encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  Result<ComputePassEncoder*> pass =
      encoder.result()->beginComputePass(ComputePassDescriptor{"convolve matrix"});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, bindGroup.result()), IsOk());
  ASSERT_THAT(pass.result()->dispatchWorkgroups(1, 1, 1), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  ASSERT_THAT(encoder.result()->copyTextureToBuffer(TexelCopyTextureInfo{output.result()},
                                                    readback.result(), {0, kBytesPerRow, kHeight},
                                                    {kWidth, kHeight}),
              IsOk());
  Result<CommandBuffer> commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  Result<uint64_t> serial = device.submit(std::move(commands).result());
  ASSERT_THAT(serial, HasResult());
  ASSERT_THAT(device.waitForSerial(serial.result(), 5.0), testing::IsTrue());
  Result<std::vector<uint8_t>> bytes = readbackBuffer(readback.result());
  ASSERT_THAT(bytes, HasResult());
  ASSERT_THAT(bytes.result(), testing::SizeIs(testing::Ge(kBytesPerRow * kHeight)));

  std::optional<tiny_skia::filter::FloatPixmap> actual =
      tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight);
  std::optional<tiny_skia::filter::FloatPixmap> expected =
      tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight);
  ASSERT_THAT(actual.has_value(), testing::IsTrue());
  ASSERT_THAT(expected.has_value(), testing::IsTrue());
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(actual->data().data() + size_t{y} * kWidth * 4,
                bytes.result().data() + size_t{y} * kBytesPerRow, kWidth * 4 * sizeof(float));
    for (uint32_t x = 0; x < kWidth; ++x) {
      const std::array<float, 4> reference = ExpectedTexel(inputTexels, params, x, y);
      std::copy(reference.begin(), reference.end(),
                expected->data().begin() + size_t{y * kWidth + x} * 4);
    }
  }
  ASSERT_THAT(actual->data(),
              testing::Each(testing::Truly([](float value) { return std::isfinite(value); })));
  const tiny_skia::Pixmap actualPixels = actual->toPixmap();
  const tiny_skia::Pixmap expectedPixels = expected->toPixmap();
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{
          Vector2i(kWidth, kHeight),
          std::vector<uint8_t>(actualPixels.data().begin(), actualPixels.data().end()), kWidth * 4},
      svg::RendererBitmap{
          Vector2i(kWidth, kHeight),
          std::vector<uint8_t>(expectedPixels.data().begin(), expectedPixels.data().end()),
          kWidth * 4},
      "convolve_edge_" + std::to_string(edgeMode) + "_preserve_" + std::to_string(preserveAlpha),
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace donner::gpu::tests
