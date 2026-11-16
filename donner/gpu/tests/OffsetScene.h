#pragma once
/// @file
/// Native offset acceptance using an independent area-weighted translation reference.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/Offset.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "tiny_skia/filter/FloatPixmap.h"
namespace donner::gpu::tests {
namespace offset_scene {
inline constexpr uint32_t kWidth = 5, kHeight = 4, kBytesPerRow = 256;
inline std::array<float, kWidth * kHeight * 4> InputTexels() {
  std::array<float, kWidth * kHeight * 4> result{};
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const size_t index = (y * kWidth + x) * 4;
      result[index] = float(1 + x) / 8.0f;
      result[index + 1] = float(1 + y) / 8.0f;
      result[index + 2] = float((x + 2 * y) % 7) / 8.0f;
      result[index + 3] = 1.0f;
    }
  }
  return result;
}
/// Texel (\p x, \p y) of \p input, or transparent black outside it.
inline std::array<float, 4> TexelOrClear(const std::array<float, kWidth * kHeight * 4>& input,
                                         int32_t x, int32_t y) {
  if (x < 0 || y < 0 || x >= int32_t(kWidth) || y >= int32_t(kHeight)) {
    return {};
  }
  std::array<float, 4> result{};
  std::copy_n(input.begin() + (y * kWidth + x) * 4, 4, result.begin());
  return result;
}
/// Exact translation by (\p dx, \p dy), computed independently of the shader's arithmetic: output
/// texel (x, y) is the area-weighted sum of the input texels that its square, moved back by the
/// shift, overlaps, evaluated in double precision.
inline std::array<float, 4> Expected(const std::array<float, kWidth * kHeight * 4>& input,
                                     double dx, double dy, int32_t x, int32_t y) {
  const auto overlap = [](double low, int32_t texel) {
    return std::max(0.0, std::min(texel + 1.0, low + 1.0) - std::max(double(texel), low));
  };
  const double left = x - dx;
  const double top = y - dy;
  std::array<double, 4> sum{};
  for (int32_t sy = int32_t(std::floor(top)); sy <= int32_t(std::floor(top)) + 1; ++sy) {
    for (int32_t sx = int32_t(std::floor(left)); sx <= int32_t(std::floor(left)) + 1; ++sx) {
      const double weight = overlap(left, sx) * overlap(top, sy);
      const auto texel = TexelOrClear(input, sx, sy);
      for (size_t channel = 0; channel < 4; ++channel) {
        sum[channel] += weight * texel[channel];
      }
    }
  }
  return {float(sum[0]), float(sum[1]), float(sum[2]), float(sum[3])};
}
}  // namespace offset_scene
/// Runs authored offset semantics with reflected bindings and workgroups on a native device.
/// @param device Native backend. @param shader Selected or test-only shader artifact.
/// @param readbackBuffer Bounded native readback. @param dx X shift. @param dy Y shift.
template <typename DeviceType, typename Readback>
void CheckOffsetStorage(DeviceType& device, const shader::CompiledShaderView& shader,
                        Readback readbackBuffer, double dx, double dy) {
  using namespace offset_scene;
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  ASSERT_EQ(shader.entryPoints.front().stage, ShaderStage::Compute);
  const auto* inputResource = shader.resource("inputTexture");
  const auto* outputResource = shader.resource("outputTexture");
  const auto* paramsResource = shader.resource("params");
  ASSERT_NE(inputResource, nullptr);
  ASSERT_NE(outputResource, nullptr);
  ASSERT_NE(paramsResource, nullptr);
  const uint32_t inputBinding = inputResource->binding, outputBinding = outputResource->binding,
                 paramsBinding = paramsResource->binding;
  const auto shape = shader.entryPoints.front().workgroupSize;
  const WorkgroupSize workgroupSize{shape[0], shape[1], shape[2]};
  const auto shaderDescriptor =
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "offset");
  const shader::programs::OffsetParams params = shader::programs::MakeOffsetParams(dx, dy);
  Result<ShaderModule> shaderModule = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shaderModule, HasResult());
  Result<BindGroupLayout> layout = device.createBindGroupLayout(BindGroupLayoutDescriptor{
      "offset",
      {{inputBinding, ShaderStage::Compute, BindingType::SampledTexture2dUnfilterableFloat},
       {outputBinding, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d,
        TextureFormat::RGBA32Float},
       {paramsBinding, ShaderStage::Compute, BindingType::UniformBuffer}}});
  ASSERT_THAT(layout, HasResult());
  Result<PipelineLayout> pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"offset", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  Result<ComputePipeline> pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "offset", pipelineLayout.result(),
      ComputeState{shaderModule.result(), RcString(shader.entryPoints.front().name.view())},
      workgroupSize});
  ASSERT_THAT(pipeline, HasResult());

  Result<Texture> input =
      device.createTexture(TextureDescriptor{"offset input",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  Result<Texture> output =
      device.createTexture(TextureDescriptor{"offset output",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(input, HasResult());
  ASSERT_THAT(output, HasResult());
  Result<TextureView> inputView = device.createTextureView(input.result(), {"offset input"});
  Result<TextureView> outputView = device.createTextureView(output.result(), {"offset output"});
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

  Result<Buffer> paramsBuffer = device.createBuffer(BufferDescriptor{
      "offset parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(paramsBuffer, HasResult());
  ASSERT_THAT(device.writeBuffer(paramsBuffer.result(), 0,
                                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&params),
                                                          sizeof(params))),
              IsOk());
  Result<BindGroup> bindGroup = device.createBindGroup(BindGroupDescriptor{
      "offset",
      layout.result(),
      {{inputBinding, TextureViewBinding{inputView.result()}},
       {outputBinding, TextureViewBinding{outputView.result()}},
       {paramsBinding, BufferBinding{paramsBuffer.result(), 0, sizeof(params)}}}});
  ASSERT_THAT(bindGroup, HasResult());

  Result<Buffer> readback =
      device.createBuffer(BufferDescriptor{"offset readback", uint64_t{kBytesPerRow} * kHeight,
                                           BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  Result<std::unique_ptr<CommandEncoder>> encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  Result<ComputePassEncoder*> pass =
      encoder.result()->beginComputePass(ComputePassDescriptor{"offset"});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, bindGroup.result()), IsOk());
  ASSERT_THAT(pass.result()->dispatchWorkgroups((kWidth + workgroupSize.x - 1) / workgroupSize.x,
                                                (kHeight + workgroupSize.y - 1) / workgroupSize.y,
                                                (1 + workgroupSize.z - 1) / workgroupSize.z),
              IsOk());
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

  auto actual = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight);
  auto expected = tiny_skia::filter::FloatPixmap::fromSize(kWidth, kHeight);
  ASSERT_TRUE(actual);
  ASSERT_TRUE(expected);
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(actual->data().data() + y * kWidth * 4, bytes.result().data() + y * kBytesPerRow,
                kWidth * 4 * sizeof(float));
    for (uint32_t x = 0; x < kWidth; ++x) {
      const auto pixel = Expected(inputTexels, dx, dy, x, y);
      std::copy(pixel.begin(), pixel.end(), expected->data().begin() + (y * kWidth + x) * 4);
    }
  }
  ASSERT_THAT(actual->data(),
              testing::Each(testing::Truly([](float value) { return std::isfinite(value); })));
  const auto actualPixels = actual->toPixmap();
  const auto expectedPixels = expected->toPixmap();
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{
          Vector2i(kWidth, kHeight),
          std::vector<uint8_t>(actualPixels.data().begin(), actualPixels.data().end()), kWidth * 4},
      svg::RendererBitmap{
          Vector2i(kWidth, kHeight),
          std::vector<uint8_t>(expectedPixels.data().begin(), expectedPixels.data().end()),
          kWidth * 4},
      "offset_" + std::to_string(dx) + "_" + std::to_string(dy),
      editor::tests::PixelmatchIdentityParams());
}
}  // namespace donner::gpu::tests
