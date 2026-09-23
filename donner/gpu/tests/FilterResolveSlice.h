#pragma once
/// @file
/// Native filter resolve acceptance with an independent lookup/quantization reference.
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
#include "donner/gpu/shader/programs/FilterResolve.h"
#include "donner/gpu/tests/GpuTestUtils.h"
namespace donner::gpu::tests {
namespace filter_resolve_slice {
inline constexpr uint32_t kWidth = 5, kHeight = 4, kBytesPerRow = 256;
inline std::array<float, kWidth * kHeight * 4> InputTexels() {
  std::array<float, kWidth * kHeight * 4> result{};
  constexpr std::array<float, 4> alphas{0.0f, 0.25f, 0.5f, 1.0f};
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const float alpha = alphas[(x + y) % 4];
      const size_t at = (y * kWidth + x) * 4;
      result[at] = alpha * float(x) / 4.0f;
      result[at + 1] = alpha * float(y) / 4.0f;
      result[at + 2] = alpha * (1.0f - float((x + y) % 4) / 4.0f);
      result[at + 3] = alpha;
    }
  }
  return result;
}
inline std::array<float, shader::programs::kFilterResolveTransferCount> TransferTable() {
  std::array<float, shader::programs::kFilterResolveTransferCount> result{};
  for (uint32_t i = 0; i < 4096; ++i) {
    result[i] = 0.375f;
    result[4096 + i] = (i % 2 == 0) ? 0.125f : 0.875f;
  }
  return result;
}
inline std::vector<uint8_t> Expected(const std::array<float, kWidth * kHeight * 4>& input,
                                     const std::array<float, 8192>& table, bool convert) {
  std::vector<uint8_t> result(kWidth * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 1; x < 4; ++x) {
      const size_t at = (y * kWidth + x) * 4;
      const float alpha = input[at + 3];
      for (uint32_t c = 0; c < 4; ++c) {
        float value = input[at + c];
        if (convert && c < 3) {
          if (alpha <= 0.0f) {
            value = 0.0f;
          } else {
            const uint32_t index =
                static_cast<uint32_t>(std::clamp(value / alpha, 0.0f, 1.0f) * 4095.0f + 0.5f);
            value = table[4096 + index] * alpha;
          }
        }
        result[at + c] = static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
      }
    }
  }
  return result;
}

}  // namespace filter_resolve_slice
/// Resolves a clipped float image through an optional transfer table using reflected resources.
/// @param device Native backend. @param shader Selected or test-only shader artifact.
/// @param readbackBuffer Bounded native readback. @param convert Whether to apply the transfer
/// table.
template <typename DeviceType, typename Readback>
void CheckFilterResolveStorage(DeviceType& device, const shader::CompiledShaderView& shader,
                               Readback readbackBuffer, bool convert) {
  using namespace filter_resolve_slice;
  ASSERT_THAT(shader.entryPoints, testing::SizeIs(1));
  ASSERT_EQ(shader.entryPoints.front().stage, ShaderStage::Compute);
  const auto* inputResource = shader.resource("inputTexture");
  const auto* outputResource = shader.resource("outputTexture");
  const auto* paramsResource = shader.resource("params");
  const auto* tableResource = shader.resource("transferTable");
  ASSERT_NE(tableResource, nullptr);
  ASSERT_NE(inputResource, nullptr);
  ASSERT_NE(outputResource, nullptr);
  ASSERT_NE(paramsResource, nullptr);
  const uint32_t inputBinding = inputResource->binding, outputBinding = outputResource->binding,
                 paramsBinding = paramsResource->binding;
  const auto shape = shader.entryPoints.front().workgroupSize;
  const WorkgroupSize workgroupSize{shape[0], shape[1], shape[2]};
  const auto shaderDescriptor =
      shader::MakeShaderDescriptor(shader, device.shaderSourceKind(), "filter_resolve");
  const shader::programs::FilterResolveParams params{
      1, 0, 0, 1, 0, 0, 1.5f, 0, 4.5f, 4, convert ? 1u : 0u, 0};
  Result<ShaderModule> shaderModule = device.createShaderModule(shaderDescriptor);
  ASSERT_THAT(shaderModule, HasResult());
  Result<BindGroupLayout> layout =
      device.createBindGroupLayout({"filter resolve", shader::MakeBindingLayout(shader)});
  ASSERT_THAT(layout, HasResult());
  Result<PipelineLayout> pipelineLayout =
      device.createPipelineLayout(PipelineLayoutDescriptor{"filter_resolve", {layout.result()}});
  ASSERT_THAT(pipelineLayout, HasResult());
  Result<ComputePipeline> pipeline = device.createComputePipeline(ComputePipelineDescriptor{
      "filter_resolve", pipelineLayout.result(),
      ComputeState{shaderModule.result(), RcString(shader.entryPoints.front().name.view())},
      workgroupSize});
  ASSERT_THAT(pipeline, HasResult());

  Result<Texture> input =
      device.createTexture(TextureDescriptor{"filter_resolve input",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA32Float,
                                             TextureUsage::Sampled | TextureUsage::CopyDst});
  Result<Texture> output =
      device.createTexture(TextureDescriptor{"filter_resolve output",
                                             {kWidth, kHeight},
                                             TextureFormat::RGBA8Unorm,
                                             TextureUsage::StorageBinding | TextureUsage::CopySrc});
  ASSERT_THAT(input, HasResult());
  ASSERT_THAT(output, HasResult());
  Result<TextureView> inputView =
      device.createTextureView(input.result(), {"filter_resolve input"});
  Result<TextureView> outputView =
      device.createTextureView(output.result(), {"filter_resolve output"});
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
      "filter_resolve parameters", sizeof(params), BufferUsage::Uniform | BufferUsage::CopyDst});
  ASSERT_THAT(paramsBuffer, HasResult());
  ASSERT_THAT(device.writeBuffer(paramsBuffer.result(), 0,
                                 std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&params),
                                                          sizeof(params))),
              IsOk());
  const auto table = TransferTable();
  auto tableBuffer = device.createBuffer(
      {"transfer table", sizeof(table), BufferUsage::Storage | BufferUsage::CopyDst});
  ASSERT_THAT(tableBuffer, HasResult());
  ASSERT_THAT(device.writeBuffer(tableBuffer.result(), 0,
                                 {reinterpret_cast<const uint8_t*>(table.data()), sizeof(table)}),
              IsOk());
  Result<BindGroup> bindGroup = device.createBindGroup(BindGroupDescriptor{
      "filter_resolve",
      layout.result(),
      {{inputBinding, TextureViewBinding{inputView.result()}},
       {outputBinding, TextureViewBinding{outputView.result()}},
       {paramsBinding, BufferBinding{paramsBuffer.result(), 0, sizeof(params)}},
       {tableResource->binding, BufferBinding{tableBuffer.result(), 0, sizeof(table)}}}});
  ASSERT_THAT(bindGroup, HasResult());

  Result<Buffer> readback = device.createBuffer(
      BufferDescriptor{"filter_resolve readback", uint64_t{kBytesPerRow} * kHeight,
                       BufferUsage::CopyDst | BufferUsage::MapRead});
  ASSERT_THAT(readback, HasResult());
  Result<std::unique_ptr<CommandEncoder>> encoder = device.createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  Result<ComputePassEncoder*> pass =
      encoder.result()->beginComputePass(ComputePassDescriptor{"filter_resolve"});
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

  std::vector<uint8_t> pixels(kWidth * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::memcpy(pixels.data() + y * kWidth * 4, bytes.result().data() + y * kBytesPerRow,
                kWidth * 4);
  }
  editor::tests::CompareBitmapToBitmap(
      svg::RendererBitmap{Vector2i(kWidth, kHeight), pixels, kWidth * 4},
      svg::RendererBitmap{Vector2i(kWidth, kHeight), Expected(inputTexels, table, convert),
                          kWidth * 4},
      "filter_resolve_convert_" + std::to_string(convert),
      editor::tests::PixelmatchIdentityParams());
}
}  // namespace donner::gpu::tests
