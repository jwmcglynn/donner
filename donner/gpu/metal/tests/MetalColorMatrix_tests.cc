/// @file
/// Runs the color-matrix kernel emitted from the shader IR on a real Metal device
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
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/shader/ModuleInterface.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/generated/ComponentTransferShader.h"
#include "donner/gpu/shader/generated/DiffuseLightingShader.h"
#include "donner/gpu/shader/generated/DisplacementMapShader.h"
#include "donner/gpu/shader/generated/DropShadowShader.h"
#include "donner/gpu/shader/generated/FilterImageShader.h"
#include "donner/gpu/shader/programs/ColorMatrix.h"
#include "donner/gpu/shader/programs/GaussianBlur.h"
#include "donner/gpu/shader/programs/Morphology.h"
#include "donner/gpu/shader/programs/SpecularLighting.h"
#include "donner/gpu/shader/programs/Tile.h"
#include "donner/gpu/shader/programs/Turbulence.h"
#include "donner/gpu/shader/tests/CompiledConvolve.h"
#include "donner/gpu/shader/tests/CompiledFilterResolve.h"
#include "donner/gpu/shader/tests/CompiledGaussian.h"
#include "donner/gpu/shader/tests/CompiledImageBlit.h"
#include "donner/gpu/shader/tests/CompiledOffset.h"
#include "donner/gpu/shader/tests/CompiledSpecularLighting.h"
#include "donner/gpu/shader/tests/CompiledTurbulence.h"
#include "donner/gpu/shader/tests/FloatStorageModule.h"
#include "donner/gpu/tests/BlurSlice.h"
#include "donner/gpu/tests/ColorMatrixSlice.h"
#include "donner/gpu/tests/ComponentTransferSlice.h"
#include "donner/gpu/tests/ConvolveMatrixSlice.h"
#include "donner/gpu/tests/DisplacementMapSlice.h"
#include "donner/gpu/tests/DropShadowSlice.h"
#include "donner/gpu/tests/FilterImageSlice.h"
#include "donner/gpu/tests/FilterResolveSlice.h"
#include "donner/gpu/tests/FloatTextureSlice.h"
#include "donner/gpu/tests/ImageBlitSlice.h"
#include "donner/gpu/tests/LightingSlice.h"
#include "donner/gpu/tests/MorphologySlice.h"
#include "donner/gpu/tests/OffsetSlice.h"
#include "donner/gpu/tests/SlugMaskSlice.h"
#include "donner/gpu/tests/TileSlice.h"
#include "donner/gpu/tests/TurbulenceSlice.h"

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
    DONNER_REQUIRE_METAL_DEVICE(device_, "the Metal color-matrix slice");
  }

  /// Unwraps an RHI result, failing the test on error.
  template <typename T>
  T unwrap(Result<T>&& result, const char* what) {
    if (result.hasError()) {
      ADD_FAILURE() << what << " failed: " << result.error();
    }
    return std::move(result).result();
  }

  /// Rebuilds the device for \p memoryModel and runs the whole color-matrix comparison against it.
  /// @param memoryModel Memory model the device builds its resources for.
  void runColorMatrixSlice(MetalDevice::MemoryModel memoryModel);

  std::unique_ptr<MetalDevice> device_;
};

void MetalColorMatrixTest::runColorMatrixSlice(MetalDevice::MemoryModel memoryModel) {
  device_ = MetalDevice::Create(memoryModel);
  ASSERT_NE(device_, nullptr);
  if (memoryModel == MetalDevice::MemoryModel::ForceNonUnified) {
    ASSERT_FALSE(device_->usesUnifiedMemoryForTest())
        << "the forced model must actually take the non-unified path";
  }

  shader::ShaderResult<shader::IrModule> irModule = shader::programs::BuildColorMatrixModule();
  ASSERT_FALSE(irModule.hasError()) << irModule.error();
  shader::ShaderResult<std::string> msl = shader::EmitMsl(irModule.result());
  ASSERT_FALSE(msl.hasError()) << msl.error();

  const auto bindings = shader::BufferBindingsOf(irModule.result());
  ASSERT_THAT(bindings, HasResult());
  ShaderModule shaderModule = unwrap(device_->createShaderModule(ShaderModuleDescriptor{
                                         "colorMatrix",
                                         RcString(msl.result()),
                                         ShaderSourceKind::Msl,
                                         {},
                                         shader::ComputeEntryPointsOf(irModule.result()),
                                         bindings.result()}),
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

TEST_F(MetalColorMatrixTest, FloatTextureDispatchPreservesSubBytePrecision) {
  const auto module = shader::BuildFloatStorageModule();
  ASSERT_FALSE(module.hasError()) << module.error();
  const auto emitted = shader::EmitMsl(module.result());
  ASSERT_FALSE(emitted.hasError()) << emitted.error();
  const auto bindings = shader::BufferBindingsOf(module.result());
  ASSERT_FALSE(bindings.hasError()) << bindings.error();
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      ShaderModuleDescriptor{"float",
                             RcString(emitted.result()),
                             ShaderSourceKind::Msl,
                             {},
                             shader::ComputeEntryPointsOf(module.result()),
                             bindings.result()},
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); });
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

class MetalFilterImageTest : public MetalColorMatrixTest,
                             public testing::WithParamInterface<uint32_t> {};

TEST_P(MetalFilterImageTest, GeneratedDescriptorMatchesIndependentReference) {
  const ShaderModuleDescriptor descriptor =
      gpu::generated::filter_image::BuildDescriptor(ShaderSourceKind::Msl);
  ASSERT_THAT(descriptor.sourceText, testing::Not(testing::IsEmpty()));
  ASSERT_THAT(descriptor.computeEntryPoints, testing::SizeIs(1u));
  for (size_t sceneIndex = 0; sceneIndex < 3; ++sceneIndex) {
    SCOPED_TRACE(testing::Message() << "sceneIndex=" << sceneIndex);
    gpu::tests::CheckFilterImageStorage(
        *device_, descriptor,
        [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, GetParam(),
        sceneIndex);
  }
}

INSTANTIATE_TEST_SUITE_P(SmoothNearestAndPixelated, MetalFilterImageTest,
                         testing::Values(0u, 1u, 2u));

TEST_F(MetalColorMatrixTest, VectorCeilAndExpRunThroughNativeCompiler) {
  const auto module = shader::BuildVectorCeilExpModule();
  ASSERT_FALSE(module.hasError()) << module.error();
  const auto emitted = shader::EmitMsl(module.result());
  ASSERT_FALSE(emitted.hasError()) << emitted.error();
  const auto bindings = shader::BufferBindingsOf(module.result());
  ASSERT_FALSE(bindings.hasError()) << bindings.error();
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      ShaderModuleDescriptor{"float",
                             RcString(emitted.result()),
                             ShaderSourceKind::Msl,
                             {},
                             shader::ComputeEntryPointsOf(module.result()),
                             bindings.result()},
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); },
      {-0.5f, 0.5f, 0.0f, 1.0f}, {0.0f, 1.0f, 16.0f, 43.0f});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, TileWrapsAndPreservesFloatStorage) {
  const auto module = shader::programs::BuildTileModule();
  ASSERT_FALSE(module.hasError()) << module.error();
  const auto emitted = shader::EmitMsl(module.result());
  ASSERT_FALSE(emitted.hasError()) << emitted.error();
  const auto bindings = shader::BufferBindingsOf(module.result());
  ASSERT_FALSE(bindings.hasError()) << bindings.error();
  gpu::tests::CheckTileStorage(
      *device_,
      ShaderModuleDescriptor{"float",
                             RcString(emitted.result()),
                             ShaderSourceKind::Msl,
                             {},
                             shader::ComputeEntryPointsOf(module.result()),
                             bindings.result()},
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); });
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, DropShadowUsesSharedInputsAndRoundsHalfOffsets) {
  gpu::tests::CheckDropShadowStorage(
      *device_, gpu::generated::drop_shadow::BuildDescriptor(ShaderSourceKind::Msl),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); });
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, DisplacementUsesGeneratedArtifactAndIndependentPixelOracle) {
  gpu::tests::CheckDisplacementMapStorage(
      *device_, gpu::generated::displacement_map::BuildDescriptor(ShaderSourceKind::Msl),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); });
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, GaussianAndBoxBlurPreservePixelsAndFoldedClip) {
  const shader::CompiledShaderView& gaussian = shader::programs::GaussianBlurNativeShader();
  const ShaderModuleDescriptor descriptor =
      shader::MakeShaderDescriptor(gaussian, ShaderSourceKind::Msl, "GaussianBlur");
  for (uint32_t axis : {0u, 1u}) {
    for (uint32_t kind : {0u, 1u, 2u}) {
      for (uint32_t edgeMode : {0u, 1u, 2u}) {
        SCOPED_TRACE(testing::Message()
                     << "axis=" << axis << " kind=" << kind << " edge=" << edgeMode);
        gpu::tests::CheckBlurStorage(
            *device_, descriptor,
            [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); },
            kind == 0 ? 0.5f : 0.0f, kind == 1 ? 1u : 0u, axis, edgeMode, &gaussian);
      }
    }
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, GaussianBlurUsesReflectedBindingAndWorkgroupMetadata) {
  const shader::CompiledShaderView& gaussian = shader::tests::GaussianBlurMutatedAllProjections();
  ASSERT_NE(gaussian.resource("params"), nullptr);
  EXPECT_EQ(gaussian.resource("params")->binding, 7u);
  ASSERT_THAT(gaussian.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(gaussian.entryPoints.front().stage, ShaderStage::Compute);
  EXPECT_EQ(gaussian.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{4, 2, 1}));

  gpu::tests::CheckBlurStorage(
      *device_,
      shader::MakeShaderDescriptor(gaussian, ShaderSourceKind::Msl, "GaussianBlurMutated"),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, 0.5f, 0u, 0u, 1u,
      &gaussian);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, MorphologyPreservesErosionDilationAndFloatStorage) {
  const auto module = shader::programs::BuildMorphologyModule();
  ASSERT_FALSE(module.hasError()) << module.error();
  const auto emitted = shader::EmitMsl(module.result());
  ASSERT_FALSE(emitted.hasError()) << emitted.error();
  const auto bindings = shader::BufferBindingsOf(module.result());
  ASSERT_FALSE(bindings.hasError()) << bindings.error();
  for (bool erode : {false, true}) {
    gpu::tests::CheckMorphologyStorage(
        *device_,
        ShaderModuleDescriptor{"float",
                               RcString(emitted.result()),
                               ShaderSourceKind::Msl,
                               {},
                               shader::ComputeEntryPointsOf(module.result()),
                               bindings.result()},
        [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, erode);
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, ComponentTransferCoversFunctionsAndPackedTableBoundaries) {
  gpu::tests::CheckComponentTransferStorage(
      *device_, gpu::generated::component_transfer::BuildDescriptor(ShaderSourceKind::Msl),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); });
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, ImageBlitNearest) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Nearest});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitLinear) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Linear});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitPixelated) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Pixelated});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitCropped) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Cropped});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitStraight) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Straight});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitPremultiplied) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Premultiplied});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitLuminanceMask) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::LuminanceMask});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitAlphaMask) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::AlphaMask});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitMaskBounds) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::MaskBounds});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitPathClip) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::PathClip});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend0) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 0u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend1) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 1u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend2) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 2u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend3) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 3u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend4) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 4u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend5) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 5u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend6) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 6u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend7) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 7u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend8) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 8u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend9) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 9u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend10) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 10u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend11) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 11u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend12) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 12u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend13) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 13u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend14) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 14u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitBlend15) {
  gpu::tests::CheckImageBlit(*device_, shader::programs::ImageBlitNativeShader(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Blend, 15u});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitMutated) {
  gpu::tests::CheckImageBlit(*device_, shader::tests::ImageBlitMutatedAllProjections(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Linear});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, ImageBlitExplicitLevel) {
  gpu::tests::CheckImageBlit(*device_, shader::tests::ImageBlitExplicitLevelAllProjections(),
                             [this](const Buffer& b) { return device_->readBackBuffer(b); },
                             {gpu::tests::image_blit_slice::Case::Linear});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, WgslArraySwitch) {
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      shader::MakeShaderDescriptor(shader::tests::ArraySwitchAllProjections(),
                                   device_->shaderSourceKind(), "ArraySwitch"),
      [this](const Buffer& b) { return device_->readBackBuffer(b); }, {0, 1, 2, -1},
      {17, 9, 11, 15});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, WgslLoopSwitch) {
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      shader::MakeShaderDescriptor(shader::tests::LoopSwitchAllProjections(),
                                   device_->shaderSourceKind(), "LoopSwitch"),
      [this](const Buffer& b) { return device_->readBackBuffer(b); }, {0.125f, 0.25f, 0.5f, 0.75f},
      {23.125f, 23.25f, 23.5f, 23.75f});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, WgslVectorMix) {
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      shader::MakeShaderDescriptor(shader::tests::VectorMixAllProjections(),
                                   device_->shaderSourceKind(), "VectorMix"),
      [this](const Buffer& b) { return device_->readBackBuffer(b); }, {0.125f, 0.25f, 0.5f, 0.75f},
      {2.3125f, 2.5f, 2.5f, 2.25f});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, TurbulencePreservesSeedsOctavesTransformsAndStitching) {
  gpu::tests::CheckTurbulenceStorage(
      *device_, shader::programs::TurbulenceNativeShader(),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); });
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, TurbulenceUsesReflectedBindingsAndWorkgroups) {
  gpu::tests::CheckTurbulenceStorage(
      *device_, shader::tests::TurbulenceMutatedAllProjections(),
      [this](const Buffer& b) { return device_->readBackBuffer(b); });
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, WgslEightArgumentCallsPreserveOperandOrder) {
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      shader::MakeShaderDescriptor(shader::tests::EightArgumentCallAllProjections(),
                                   device_->shaderSourceKind(), "eight arguments"),
      [this](const Buffer& b) { return device_->readBackBuffer(b); }, {0.125f, 0.25f, 0.5f, 0.75f},
      {114.125f, 114.125f, 114.125f, 114.125f});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, WgslLightingMathPreservesVectorLanes) {
  // Quantization isolates lane/opcode correctness from backend transcendental approximation.
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      shader::MakeShaderDescriptor(shader::tests::LightingMathAllProjections(),
                                   device_->shaderSourceKind(), "lighting vector math"),
      [this](const Buffer& b) { return device_->readBackBuffer(b); },
      {0.125f, 0.25f, 0.625f, 0.875f}, {1.6875f, 1.75f, 2.8125f, 4.9375f});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, SpecularLightingUsesReflectedBindingAndWorkgroup) {
  const auto& shader = shader::tests::SpecularLightingMutatedAllProjections();
  const auto descriptor =
      shader::MakeShaderDescriptor(shader, device_->shaderSourceKind(), "mutated lighting");
  gpu::tests::CheckLightingStorage(
      *device_, descriptor,
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, true, 2, &shader);
}

TEST_F(MetalColorMatrixTest, SpecularLightingDefinesZeroToZeroAsOne) {
  const auto& shader = shader::programs::SpecularLightingNativeShader();
  auto params = gpu::tests::lighting_detail::MakeParams(true, 1);
  params.surfaceScale = 0;
  params.lightX = 2;
  params.lightY = 2;
  params.lightZ = -2;
  params.specularExponent = 0;
  const auto descriptor =
      shader::MakeShaderDescriptor(shader, device_->shaderSourceKind(), "zero exponent lighting");
  gpu::tests::CheckLightingStorage(
      *device_, descriptor,
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, true, 1, &shader,
      &params);
}

TEST_F(MetalColorMatrixTest, LightingArtifactsPreserveAllLightSourcesAndFloatStorage) {
  for (bool specular : {false, true}) {
    const ShaderModuleDescriptor descriptor =
        specular ? shader::MakeShaderDescriptor(shader::programs::SpecularLightingNativeShader(),
                                                device_->shaderSourceKind(), "specular lighting")
                 : generated::diffuse_lighting::BuildDescriptor(device_->shaderSourceKind());
    for (uint32_t lightType : {0u, 1u, 2u}) {
      SCOPED_TRACE(testing::Message() << "specular=" << specular << " light=" << lightType);
      gpu::tests::CheckLightingStorage(
          *device_, descriptor,
          [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, specular,
          lightType, specular ? &shader::programs::SpecularLightingNativeShader() : nullptr);
    }
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, FilterResolveClipsTransfersAndQuantizes) {
  for (bool convert : {false, true}) {
    gpu::tests::CheckFilterResolveStorage(
        *device_, shader::programs::FilterResolveNativeShader(),
        [this](const Buffer& b) { return device_->readBackBuffer(b); }, convert);
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, FilterResolveUsesReflectedTableBindingAndWorkgroups) {
  gpu::tests::CheckFilterResolveStorage(
      *device_, shader::tests::FilterResolveMutatedAllProjections(),
      [this](const Buffer& b) { return device_->readBackBuffer(b); }, true);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, OffsetPreservesRoundingAndTransparentEdges) {
  const auto& shader = shader::programs::OffsetNativeShader();
  const std::array<std::array<float, 2>, 10> shifts{
      {{0, 0},
       {2, -3},
       {0.5f, -0.5f},
       {-0.5f, 0.5f},
       {2.5f, -2.5f},
       {3.5f, -3.5f},
       {4096, 0},
       {0, -4096},
       {std::nextafter(0.5f, 0.0f), std::nextafter(-0.5f, 0.0f)},
       {std::nextafter(0.5f, 1.0f), std::nextafter(-0.5f, -1.0f)}}};
  for (const auto& shift : shifts) {
    gpu::tests::CheckOffsetStorage(
        *device_, shader, [this](const Buffer& b) { return device_->readBackBuffer(b); }, shift[0],
        shift[1]);
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, OffsetUsesChangedBindingAndWorkgroupShape) {
  gpu::tests::CheckOffsetStorage(
      *device_, shader::tests::OffsetMutatedAllProjections(),
      [this](const Buffer& b) { return device_->readBackBuffer(b); }, 0.5f, -0.5f);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}
TEST_F(MetalColorMatrixTest, WgslFloorAndSignPreserveVectorLanes) {
  const std::array<float, 4> values{-1.5f, -0.0f, 0.25f, 1.5f};
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      shader::MakeShaderDescriptor(shader::tests::FloorAllProjections(),
                                   device_->shaderSourceKind(), "floor"),
      [this](const Buffer& b) { return device_->readBackBuffer(b); }, values,
      {-2.0f, 0.0f, 0.0f, 1.0f});
  gpu::tests::CheckFloatTextureStorage(
      *device_,
      shader::MakeShaderDescriptor(shader::tests::SignAllProjections(), device_->shaderSourceKind(),
                                   "sign"),
      [this](const Buffer& b) { return device_->readBackBuffer(b); }, values,
      {-1.0f, 0.0f, 1.0f, 1.0f});
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, ConvolveMatrixPreservesSvgSamplingAndAlphaSemantics) {
  const shader::CompiledShaderView& convolve = shader::programs::ConvolveMatrixNativeShader();
  const ShaderModuleDescriptor descriptor =
      shader::MakeShaderDescriptor(convolve, ShaderSourceKind::Msl, "ConvolveMatrix");
  for (uint32_t edgeMode : {0u, 1u, 2u}) {
    for (bool preserveAlpha : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "edgeMode=" << edgeMode << " preserveAlpha=" << preserveAlpha);
      gpu::tests::CheckConvolveMatrixStorage(
          *device_, descriptor,
          [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, edgeMode,
          preserveAlpha, gpu::tests::convolve_matrix_slice::ArrayIndexMode::Authored, &convolve);
    }
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, ConvolveMatrixUsesReflectedBindingAndWorkgroupMetadata) {
  const shader::CompiledShaderView& convolve = shader::tests::ConvolveMatrixMutatedAllProjections();
  ASSERT_NE(convolve.resource("params"), nullptr);
  EXPECT_EQ(convolve.resource("params")->binding, 7u);
  ASSERT_THAT(convolve.entryPoints, testing::SizeIs(1));
  EXPECT_EQ(convolve.entryPoints.front().stage, ShaderStage::Compute);
  EXPECT_EQ(convolve.entryPoints.front().workgroupSize, (std::array<uint32_t, 3>{4, 2, 1}));
  gpu::tests::CheckConvolveMatrixStorage(
      *device_,
      shader::MakeShaderDescriptor(convolve, ShaderSourceKind::Msl, "ConvolveMatrixMutated"),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, 1u, true,
      gpu::tests::convolve_matrix_slice::ArrayIndexMode::Authored, &convolve);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, ConvolveMatrixClampsHighAndLowStorageIndices) {
  for (const auto [convolve, mode] :
       {std::pair{&shader::tests::ConvolveMatrixHighIndexAllProjections(),
                  gpu::tests::convolve_matrix_slice::ArrayIndexMode::High},
        std::pair{&shader::tests::ConvolveMatrixLowIndexAllProjections(),
                  gpu::tests::convolve_matrix_slice::ArrayIndexMode::Low}}) {
    gpu::tests::CheckConvolveMatrixStorage(
        *device_,
        shader::MakeShaderDescriptor(*convolve, ShaderSourceKind::Msl, "ConvolveMatrixIndex"),
        [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); }, 1u, true, mode,
        convolve);
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, DispatchMatchesTheHostComputedResult) {
  runColorMatrixSlice(MetalDevice::MemoryModel::Detected);
}

TEST_F(MetalColorMatrixTest, UnifiedMemoryPublishesNothingBecauseThereIsOneCopy) {
  runColorMatrixSlice(MetalDevice::MemoryModel::Detected);
  if (!device_->usesUnifiedMemoryForTest()) {
    GTEST_SKIP() << "this machine reports non-unified memory; the forced case below covers it";
  }
  EXPECT_EQ(device_->hostWritePublishCountForTest(), 0u)
      << "both processors address one copy, so there is nothing to publish";
  EXPECT_EQ(device_->deviceWritePublishCountForTest(), 0u);
}

// This test is the only one that runs under both memory models. The solid-fill and
// sub-rectangle-copy tests run under the detected one, which is unified on every machine
// available to run them, and they reach the host through the same publication the counters below
// pin - so the mechanism is covered once here rather than repeated per test.
TEST_F(MetalColorMatrixTest, WithoutUnifiedMemoryEveryChangeIsPublishedInBothDirections) {
  // The behaviour this fix exists for cannot be observed in the pixels on hardware that
  // addresses one copy of a resource: the results come out right whether or not anything was
  // published, which is exactly why a virtualized device caught what every local run missed. So
  // the assertion is on the publication itself, which is the mechanism the virtualized device
  // needs and the part that was absent.
  runColorMatrixSlice(MetalDevice::MemoryModel::ForceNonUnified);

  EXPECT_GT(device_->hostWritePublishCountForTest(), 0u)
      << "a host write the device never sees published leaves it reading a stale copy";
  EXPECT_GT(device_->deviceWritePublishCountForTest(), 0u)
      << "output the host reads must be published back before the submission completes, or the "
         "read returns whatever the host copy last held";
}

TEST_F(MetalColorMatrixTest, DispatchMatchesTheHostComputedResultWithoutUnifiedMemory) {
  // The same comparison built for a device that does not address one copy of a resource from both
  // processors. On such a device the host's copy of the readback buffer is only whatever was
  // published to it, so a dispatch whose output is never published reads back as zeros - which
  // is what a virtualized Metal device shows and what unified-memory hardware hides. Forcing the
  // model here is what puts that path under test on hardware that would never take it.
  runColorMatrixSlice(MetalDevice::MemoryModel::ForceNonUnified);
}

TEST_F(MetalColorMatrixTest, SlugMaskAnalyticRectangle) {
  gpu::tests::CheckSlugMask(
      *device_, shader::programs::SlugMaskNativeShader(),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); },
      gpu::tests::slug_mask_slice::Case::Analytic);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, SlugMaskBinaryRectangle) {
  gpu::tests::CheckSlugMask(
      *device_, shader::programs::SlugMaskNativeShader(),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); },
      gpu::tests::slug_mask_slice::Case::Binary);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, SlugMaskNestedClipCoverage) {
  gpu::tests::CheckSlugMask(
      *device_, shader::programs::SlugMaskNativeShader(),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); },
      gpu::tests::slug_mask_slice::Case::NestedClip);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, SlugMaskDoubleNonzeroWinding) {
  gpu::tests::CheckSlugMask(
      *device_, shader::programs::SlugMaskNativeShader(),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); },
      gpu::tests::slug_mask_slice::Case::DoubleNonzero);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, SlugMaskDoubleEvenOddWinding) {
  gpu::tests::CheckSlugMask(
      *device_, shader::programs::SlugMaskNativeShader(),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); },
      gpu::tests::slug_mask_slice::Case::DoubleEvenOdd);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalColorMatrixTest, SlugMaskStorageReadsRespectDeclaredRange) {
  gpu::tests::CheckSlugMask(
      *device_, shader::programs::SlugMaskNativeShader(),
      [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); },
      gpu::tests::slug_mask_slice::Case::DeclaredRange);
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

}  // namespace
}  // namespace donner::gpu::metal::tests
