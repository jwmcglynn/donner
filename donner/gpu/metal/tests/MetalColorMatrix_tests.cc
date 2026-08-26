/// @file
/// The Metal compute vertical slice: runs the color-matrix kernel emitted from the shader IR
/// through donner::gpu::metal::MetalDevice and compares the destination texels byte-for-byte
/// against the same result computed on the host.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/programs/ColorMatrix.h"
#include "donner/gpu/tests/ColorMatrixSlice.h"

namespace donner::gpu::metal::tests {
namespace {

using gpu::tests::ColorMatrixBias;
using gpu::tests::ColorMatrixExpectedTexel;
using gpu::tests::ColorMatrixInputTexels;
using gpu::tests::ColorMatrixParams;
using gpu::tests::ColorMatrixUniforms;
using gpu::tests::ColorMatrixWorkgroupCount;
using gpu::tests::kColorMatrixBytesPerRow;
using gpu::tests::kColorMatrixHeight;
using gpu::tests::kColorMatrixWidth;
using shader::programs::ColorMatrixBinding;
using shader::programs::kColorMatrixWorkgroupSize;

/// Binding index of \p binding as the runtime takes it.
uint32_t BindingIndex(ColorMatrixBinding binding) {
  return static_cast<uint32_t>(binding);
}

/// Bytes of \p value as a span, for queue writes of a host struct.
template <typename T>
std::span<const uint8_t> AsBytes(const T& value) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

class MetalColorMatrixTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    if (!device_) {
      GTEST_SKIP() << "No Metal device available";
    }
  }

  /// Unwraps an RHI result, failing the test on error.
  template <typename T>
  T unwrap(Result<T>&& result, const char* what) {
    if (result.hasError()) {
      ADD_FAILURE() << what << " failed: " << result.error();
    }
    return std::move(result).result();
  }

  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalColorMatrixTest, DispatchMatchesTheHostComputedResult) {
  shader::ShaderResult<shader::IrModule> irModule = shader::programs::BuildColorMatrixModule();
  ASSERT_FALSE(irModule.hasError()) << irModule.error();
  shader::ShaderResult<std::string> msl = shader::EmitMsl(irModule.result());
  ASSERT_FALSE(msl.hasError()) << msl.error();

  ShaderModule shaderModule = unwrap(device_->createShaderModule(ShaderModuleDescriptor{
                                         "colorMatrix",
                                         RcString(msl.result()),
                                         ShaderSourceKind::Msl,
                                         {},
                                         shader::ComputeEntryPointsOf(irModule.result())}),
                                     "createShaderModule");

  const std::vector<BindGroupLayoutEntry> layoutEntries = {
      {BindingIndex(ColorMatrixBinding::InputTexture), ShaderStage::Compute,
       BindingType::SampledTexture2dFloat},
      {BindingIndex(ColorMatrixBinding::OutputTexture), ShaderStage::Compute,
       BindingType::WriteOnlyStorageTexture2d, TextureFormat::RGBA8Unorm},
      {BindingIndex(ColorMatrixBinding::Params), ShaderStage::Compute, BindingType::UniformBuffer},
      {BindingIndex(ColorMatrixBinding::Bias), ShaderStage::Compute,
       BindingType::ReadOnlyStorageBuffer}};
  BindGroupLayout bindGroupLayout = unwrap(
      device_->createBindGroupLayout(BindGroupLayoutDescriptor{"colorMatrixBGL", layoutEntries}),
      "createBindGroupLayout");
  PipelineLayout pipelineLayout = unwrap(
      device_->createPipelineLayout(PipelineLayoutDescriptor{"colorMatrixPL", {bindGroupLayout}}),
      "createPipelineLayout");
  ComputePipeline pipeline =
      unwrap(device_->createComputePipeline(ComputePipelineDescriptor{
                 "colorMatrix", pipelineLayout, ComputeState{shaderModule, "cs_main"},
                 WorkgroupSize{kColorMatrixWorkgroupSize, kColorMatrixWorkgroupSize, 1}}),
             "createComputePipeline");

  // ----- Resources -----
  const Extent2d extent{kColorMatrixWidth, kColorMatrixHeight};
  Texture input = unwrap(
      device_->createTexture(TextureDescriptor{"input", extent, TextureFormat::RGBA8Unorm,
                                               TextureUsage::Sampled | TextureUsage::CopyDst}),
      "createTexture input");
  TextureView inputView = unwrap(
      device_->createTextureView(input, TextureViewDescriptor{"inputView"}), "createTextureView");
  Texture output = unwrap(device_->createTexture(TextureDescriptor{
                              "output", extent, TextureFormat::RGBA8Unorm,
                              TextureUsage::StorageBinding | TextureUsage::CopySrc}),
                          "createTexture output");
  TextureView outputView =
      unwrap(device_->createTextureView(output, TextureViewDescriptor{"outputView"}),
             "createTextureView output");

  // writeTexture requires a 256-aligned row pitch, so the input rows are repacked into a padded
  // staging buffer before upload.
  const std::vector<uint8_t> inputTexels = ColorMatrixInputTexels();
  std::vector<uint8_t> paddedInput(size_t{kColorMatrixBytesPerRow} * kColorMatrixHeight, 0);
  for (uint32_t y = 0; y < kColorMatrixHeight; ++y) {
    std::memcpy(paddedInput.data() + size_t{y} * kColorMatrixBytesPerRow,
                inputTexels.data() + size_t{y} * kColorMatrixWidth * 4u, kColorMatrixWidth * 4u);
  }
  ASSERT_FALSE(
      device_
          ->writeTexture(input, paddedInput,
                         TexelCopyBufferLayout{0, kColorMatrixBytesPerRow, kColorMatrixHeight},
                         extent)
          .hasError());

  const ColorMatrixParams uniforms = ColorMatrixUniforms();
  Buffer paramsBuffer =
      unwrap(device_->createBuffer(BufferDescriptor{"params", sizeof(ColorMatrixParams),
                                                    BufferUsage::Uniform | BufferUsage::CopyDst}),
             "createBuffer params");
  ASSERT_FALSE(device_->writeBuffer(paramsBuffer, 0, AsBytes(uniforms)).hasError());

  const std::array<float, 8> bias = ColorMatrixBias();
  Buffer biasBuffer =
      unwrap(device_->createBuffer(BufferDescriptor{"bias", sizeof(bias),
                                                    BufferUsage::Storage | BufferUsage::CopyDst}),
             "createBuffer bias");
  ASSERT_FALSE(device_->writeBuffer(biasBuffer, 0, AsBytes(bias)).hasError());

  Buffer readback = unwrap(device_->createBuffer(BufferDescriptor{
                               "readback", uint64_t{kColorMatrixBytesPerRow} * kColorMatrixHeight,
                               BufferUsage::CopyDst | BufferUsage::MapRead}),
                           "createBuffer readback");

  BindGroup bindGroup =
      unwrap(device_->createBindGroup(BindGroupDescriptor{
                 "colorMatrixGroup",
                 bindGroupLayout,
                 {BindGroupEntry{BindingIndex(ColorMatrixBinding::InputTexture),
                                 TextureViewBinding{inputView}},
                  BindGroupEntry{BindingIndex(ColorMatrixBinding::OutputTexture),
                                 TextureViewBinding{outputView}},
                  BindGroupEntry{BindingIndex(ColorMatrixBinding::Params),
                                 BufferBinding{paramsBuffer, 0, sizeof(ColorMatrixParams)}},
                  BindGroupEntry{BindingIndex(ColorMatrixBinding::Bias),
                                 BufferBinding{biasBuffer, 0, sizeof(bias)}}}}),
             "createBindGroup");

  // ----- Dispatch and read back -----
  std::unique_ptr<CommandEncoder> encoder =
      unwrap(device_->createCommandEncoder(), "createCommandEncoder");
  Result<ComputePassEncoder*> passResult =
      encoder->beginComputePass(ComputePassDescriptor{"colorMatrixPass"});
  ASSERT_FALSE(passResult.hasError()) << passResult.error();
  ComputePassEncoder* pass = passResult.result();

  ASSERT_FALSE(pass->setPipeline(pipeline).hasError());
  ASSERT_FALSE(pass->setBindGroup(0, bindGroup).hasError());
  ASSERT_FALSE(pass->dispatchWorkgroups(
                       ColorMatrixWorkgroupCount(kColorMatrixWidth, kColorMatrixWorkgroupSize),
                       ColorMatrixWorkgroupCount(kColorMatrixHeight, kColorMatrixWorkgroupSize), 1)
                   .hasError());
  ASSERT_FALSE(pass->end().hasError());
  ASSERT_FALSE(encoder
                   ->copyTextureToBuffer(
                       TexelCopyTextureInfo{output}, readback,
                       TexelCopyBufferLayout{0, kColorMatrixBytesPerRow, kColorMatrixHeight},
                       extent)
                   .hasError());

  Result<CommandBuffer> commands = encoder->finish();
  ASSERT_FALSE(commands.hasError()) << commands.error();
  Result<uint64_t> serial = device_->submit(std::move(commands).result());
  ASSERT_FALSE(serial.hasError()) << serial.error();

  ASSERT_TRUE(device_->waitForSerial(serial.result(), /*timeoutSeconds=*/30.0))
      << "Command buffer did not complete cleanly: " << device_->lastErrorForTest();
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());

  Result<std::vector<uint8_t>> pixels = device_->readBackBuffer(readback);
  ASSERT_FALSE(pixels.hasError()) << pixels.error();

  // The arithmetic is exact in unorm8 (see ColorMatrixSlice.h), so every texel must match the
  // host result byte-for-byte.
  for (uint32_t y = 0; y < kColorMatrixHeight; ++y) {
    for (uint32_t x = 0; x < kColorMatrixWidth; ++x) {
      const size_t offset = size_t{y} * kColorMatrixBytesPerRow + size_t{x} * 4u;
      const std::array<uint8_t, 4> actual = {
          pixels.result()[offset + 0], pixels.result()[offset + 1], pixels.result()[offset + 2],
          pixels.result()[offset + 3]};
      EXPECT_THAT(actual, testing::ElementsAreArray(ColorMatrixExpectedTexel(x, y)))
          << "texel (" << x << ", " << y << ")";
    }
  }
}

}  // namespace
}  // namespace donner::gpu::metal::tests
