/// @file
/// Accept/reject tests for every \ref donner::gpu::Device descriptor validator.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::HasSubstr;

namespace donner::gpu {
namespace {

std::vector<uint8_t> MakeBytes(size_t count) {
  std::vector<uint8_t> bytes(count);
  for (size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<uint8_t>(i & 0xFF);
  }
  return bytes;
}

class DeviceValidationTests : public testing::Test {
protected:
  Buffer createUniformBuffer(uint64_t byteSize = 16) {
    return GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"uniform", byteSize, BufferUsage::Uniform | BufferUsage::CopyDst}));
  }

  RecordingDevice device_;
};

// == createBuffer =============================================================================

TEST_F(DeviceValidationTests, CreateBufferAcceptsValidDescriptor) {
  EXPECT_THAT(device_.createBuffer(BufferDescriptor{"vertices", 48, BufferUsage::Vertex}),
              HasResult());
}

TEST_F(DeviceValidationTests, CreateBufferRejectsZeroSize) {
  EXPECT_THAT(device_.createBuffer(BufferDescriptor{"empty", 0, BufferUsage::Vertex}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("byteSize is 0")));
}

TEST_F(DeviceValidationTests, CreateBufferRejectsEmptyUsage) {
  EXPECT_THAT(device_.createBuffer(BufferDescriptor{"unusable", 16, BufferUsage::None}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("usage is empty")));
}

TEST_F(DeviceValidationTests, CreateBufferRejectsOversizedBuffer) {
  EXPECT_THAT(
      device_.createBuffer(BufferDescriptor{"huge", kMaxBufferByteSize + 1, BufferUsage::Vertex}),
      IsGpuError(GpuErrorType::LimitExceeded));
}

// == createTexture ============================================================================

TEST_F(DeviceValidationTests, CreateTextureAcceptsValidDescriptor) {
  EXPECT_THAT(
      device_.createTexture(TextureDescriptor{"target", Extent2d{16, 16}, TextureFormat::RGBA8Unorm,
                                              TextureUsage::RenderAttachment}),
      HasResult());
}

TEST_F(DeviceValidationTests, CreateTextureRejectsZeroDimension) {
  EXPECT_THAT(device_.createTexture(TextureDescriptor{
                  "flat", Extent2d{16, 0}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("zero dimension")));
}

TEST_F(DeviceValidationTests, CreateTextureRejectsOversizedDimension) {
  EXPECT_THAT(
      device_.createTexture(TextureDescriptor{"vast", Extent2d{kMaxTextureDimension + 1, 4},
                                              TextureFormat::RGBA8Unorm, TextureUsage::Sampled}),
      IsGpuError(GpuErrorType::LimitExceeded));
}

TEST_F(DeviceValidationTests, CreateTextureRejectsEmptyUsage) {
  EXPECT_THAT(device_.createTexture(TextureDescriptor{
                  "unusable", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::None}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(DeviceValidationTests, CreateTextureRejectsMultisample) {
  EXPECT_THAT(device_.createTexture(TextureDescriptor{
                  "msaa", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment,
                  /*sampleCount=*/4}),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("sampleCount 4")));
}

// == createTextureView ========================================================================

TEST_F(DeviceValidationTests, CreateTextureViewAcceptsLiveTexture) {
  const Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  EXPECT_THAT(device_.createTextureView(texture, TextureViewDescriptor{"view"}), HasResult());
}

TEST_F(DeviceValidationTests, CreateTextureViewRejectsNullTexture) {
  EXPECT_THAT(device_.createTextureView(Texture(), TextureViewDescriptor{"view"}),
              IsGpuError(GpuErrorType::InvalidHandle));
}

// == createBindGroupLayout ====================================================================

TEST_F(DeviceValidationTests, CreateBindGroupLayoutAcceptsValidDescriptor) {
  EXPECT_THAT(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
                  "uniforms",
                  {BindGroupLayoutEntry{0, ShaderStage::Vertex | ShaderStage::Fragment,
                                        BindingType::UniformBuffer}}}),
              HasResult());
}

TEST_F(DeviceValidationTests, CreateBindGroupLayoutRejectsEmptyEntries) {
  EXPECT_THAT(device_.createBindGroupLayout(BindGroupLayoutDescriptor{"empty", {}}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(DeviceValidationTests, CreateBindGroupLayoutRejectsDuplicateBindings) {
  EXPECT_THAT(
      device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "duplicate",
          {BindGroupLayoutEntry{1, ShaderStage::Fragment, BindingType::UniformBuffer},
           BindGroupLayoutEntry{1, ShaderStage::Fragment, BindingType::FilteringSampler}}}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("duplicate binding")));
}

TEST_F(DeviceValidationTests, CreateBindGroupLayoutRejectsTooManyBindings) {
  std::vector<BindGroupLayoutEntry> entries;
  for (uint32_t i = 0; i < kMaxBindings + 1; ++i) {
    entries.push_back(BindGroupLayoutEntry{i, ShaderStage::Fragment, BindingType::UniformBuffer});
  }
  EXPECT_THAT(device_.createBindGroupLayout(BindGroupLayoutDescriptor{"many", entries}),
              IsGpuError(GpuErrorType::LimitExceeded));
}

TEST_F(DeviceValidationTests, CreateBindGroupLayoutRejectsOutOfRangeBindingIndex) {
  EXPECT_THAT(
      device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "outOfRange",
          {BindGroupLayoutEntry{kMaxBindings, ShaderStage::Fragment, BindingType::UniformBuffer}}}),
      IsGpuError(GpuErrorType::LimitExceeded));
}

TEST_F(DeviceValidationTests, CreateBindGroupLayoutRejectsEmptyVisibility) {
  EXPECT_THAT(
      device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "invisible", {BindGroupLayoutEntry{0, ShaderStage::None, BindingType::UniformBuffer}}}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("empty visibility")));
}

// == createPipelineLayout =====================================================================

TEST_F(DeviceValidationTests, CreatePipelineLayoutAcceptsValidDescriptor) {
  const BindGroupLayout layout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "uniforms", {BindGroupLayoutEntry{0, ShaderStage::Vertex, BindingType::UniformBuffer}}}));
  EXPECT_THAT(device_.createPipelineLayout(PipelineLayoutDescriptor{"layout", {layout}}),
              HasResult());
}

TEST_F(DeviceValidationTests, CreatePipelineLayoutRejectsTooManyGroups) {
  const BindGroupLayout layout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "uniforms", {BindGroupLayoutEntry{0, ShaderStage::Vertex, BindingType::UniformBuffer}}}));
  const std::vector<BindGroupLayoutRef> groups(kMaxBindGroups + 1, BindGroupLayoutRef(layout));
  EXPECT_THAT(device_.createPipelineLayout(PipelineLayoutDescriptor{"many", groups}),
              IsGpuError(GpuErrorType::LimitExceeded));
}

TEST_F(DeviceValidationTests, CreatePipelineLayoutRejectsStaleLayout) {
  const BindGroupLayout layout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "uniforms", {BindGroupLayoutEntry{0, ShaderStage::Vertex, BindingType::UniformBuffer}}}));
  EXPECT_THAT(device_.destroyBindGroupLayout(layout), IsOk());
  EXPECT_THAT(device_.createPipelineLayout(PipelineLayoutDescriptor{"stale", {layout}}),
              IsGpuError(GpuErrorType::InvalidHandle));
}

// == createShaderModule =======================================================================

TEST_F(DeviceValidationTests, CreateShaderModuleAcceptsValidDescriptor) {
  EXPECT_THAT(device_.createShaderModule(ShaderModuleDescriptor{"solid", "@vertex fn vsMain() {}",
                                                                ShaderSourceKind::Wgsl}),
              HasResult());
}

TEST_F(DeviceValidationTests, CreateShaderModuleRejectsEmptySource) {
  EXPECT_THAT(
      device_.createShaderModule(ShaderModuleDescriptor{"empty", ""}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("sourceText is empty")));
}

// == createBindGroup ==========================================================================

class BindGroupValidationTests : public DeviceValidationTests {
protected:
  void SetUp() override {
    uniformLayout_ = GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
        "uniforms", {BindGroupLayoutEntry{0, ShaderStage::Vertex, BindingType::UniformBuffer}}}));
  }

  BindGroupLayout uniformLayout_;
};

TEST_F(BindGroupValidationTests, AcceptsMatchingEntries) {
  const Buffer uniform = createUniformBuffer();
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", uniformLayout_, {BindGroupEntry{0, BufferBinding{uniform, 0, 16}}}}),
              HasResult());
}

TEST_F(BindGroupValidationTests, RejectsNullBufferHandle) {
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", uniformLayout_, {BindGroupEntry{0, BufferBinding{BufferRef(), 0, 16}}}}),
              IsGpuError(GpuErrorType::InvalidHandle));
}

TEST_F(BindGroupValidationTests, RejectsBindingTypeMismatch) {
  const Sampler sampler = GetResultOrFail(device_.createSampler(SamplerDescriptor{"sampler"}));
  EXPECT_THAT(
      device_.createBindGroup(BindGroupDescriptor{
          "group", uniformLayout_, {BindGroupEntry{0, SamplerBinding{sampler}}}}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("must bind a buffer")));
}

TEST_F(BindGroupValidationTests, RejectsMissingBinding) {
  const Buffer uniform = createUniformBuffer();
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", uniformLayout_, {BindGroupEntry{7, BufferBinding{uniform, 0, 16}}}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("missing an entry for layout binding 0")));
}

TEST_F(BindGroupValidationTests, RejectsEntryCountMismatch) {
  const Buffer uniform = createUniformBuffer();
  EXPECT_THAT(
      device_.createBindGroup(
          BindGroupDescriptor{"group",
                              uniformLayout_,
                              {BindGroupEntry{0, BufferBinding{uniform, 0, 8}},
                               BindGroupEntry{1, BufferBinding{uniform, 8, 8}}}}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("layout requires 1")));
}

TEST_F(BindGroupValidationTests, RejectsBufferWithoutUniformUsage) {
  const Buffer vertexOnly =
      GetResultOrFail(device_.createBuffer(BufferDescriptor{"vertices", 16, BufferUsage::Vertex}));
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", uniformLayout_, {BindGroupEntry{0, BufferBinding{vertexOnly, 0, 16}}}}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("Uniform usage")));
}

TEST_F(BindGroupValidationTests, RejectsOutOfBoundsBufferRange) {
  const Buffer uniform = createUniformBuffer(/*byteSize=*/16);
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", uniformLayout_, {BindGroupEntry{0, BufferBinding{uniform, 8, 16}}}}),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(BindGroupValidationTests, RejectsZeroSizeBufferRange) {
  const Buffer uniform = createUniformBuffer();
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", uniformLayout_, {BindGroupEntry{0, BufferBinding{uniform, 0, 0}}}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("sizeBytes is 0")));
}

TEST_F(BindGroupValidationTests, RejectsTextureViewWithoutSampledUsage) {
  const BindGroupLayout textureLayout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "texture",
          {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat}}}));
  const Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"view"}));

  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", textureLayout, {BindGroupEntry{0, TextureViewBinding{view}}}}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("Sampled usage")));
}

// == createRenderPipeline =====================================================================

class RenderPipelineValidationTests : public DeviceValidationTests {
protected:
  void SetUp() override {
    layout_ = GetResultOrFail(device_.createPipelineLayout(PipelineLayoutDescriptor{"empty", {}}));
    shader_ = GetResultOrFail(device_.createShaderModule(
        ShaderModuleDescriptor{"solid", "@vertex fn vsMain() {}", ShaderSourceKind::Wgsl}));
  }

  RenderPipelineDescriptor validDescriptor() const {
    return RenderPipelineDescriptor{
        "solid", layout_,
        VertexState{
            shader_,
            "vsMain",
            {VertexBufferLayout{
                8, VertexStepMode::Vertex, {VertexAttribute{VertexFormat::Float32x2, 0, 0}}}}},
        FragmentState{shader_, "fsMain", {ColorTargetState{TextureFormat::RGBA8Unorm}}}};
  }

  PipelineLayout layout_;
  ShaderModule shader_;
};

TEST_F(RenderPipelineValidationTests, AcceptsValidDescriptor) {
  EXPECT_THAT(device_.createRenderPipeline(validDescriptor()), HasResult());
}

TEST_F(RenderPipelineValidationTests, RejectsAttributeBeyondStride) {
  RenderPipelineDescriptor descriptor = validDescriptor();
  descriptor.vertex.buffers[0].attributes[0].offsetBytes = 4;  // 4 + 8 > stride 8.
  EXPECT_THAT(
      device_.createRenderPipeline(descriptor),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("overflows strideBytes 8")));
}

TEST_F(RenderPipelineValidationTests, RejectsDuplicateShaderLocations) {
  RenderPipelineDescriptor descriptor = validDescriptor();
  descriptor.vertex.buffers[0].strideBytes = 16;
  descriptor.vertex.buffers[0].attributes.push_back(VertexAttribute{VertexFormat::Float32x2, 8, 0});
  EXPECT_THAT(device_.createRenderPipeline(descriptor),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("duplicate vertex shaderLocation 0")));
}

TEST_F(RenderPipelineValidationTests, RejectsEmptyVertexEntryPoint) {
  RenderPipelineDescriptor descriptor = validDescriptor();
  descriptor.vertex.entryPoint = "";
  EXPECT_THAT(device_.createRenderPipeline(descriptor),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("vertex.entryPoint is empty")));
}

TEST_F(RenderPipelineValidationTests, RejectsEmptyTargets) {
  RenderPipelineDescriptor descriptor = validDescriptor();
  descriptor.fragment.targets.clear();
  EXPECT_THAT(
      device_.createRenderPipeline(descriptor),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("targets is empty")));
}

TEST_F(RenderPipelineValidationTests, RejectsTooManyVertexAttributes) {
  RenderPipelineDescriptor descriptor = validDescriptor();
  VertexBufferLayout& layout = descriptor.vertex.buffers[0];
  layout.strideBytes = (kMaxVertexAttributes + 1) * 8;
  layout.attributes.clear();
  for (uint32_t i = 0; i < kMaxVertexAttributes + 1; ++i) {
    layout.attributes.push_back(VertexAttribute{VertexFormat::Float32x2, i * 8, i});
  }
  EXPECT_THAT(
      device_.createRenderPipeline(descriptor),
      IsGpuErrorWithMessage(GpuErrorType::LimitExceeded, HasSubstr("kMaxVertexAttributes")));
}

TEST_F(RenderPipelineValidationTests, RejectsOutOfRangeShaderLocation) {
  RenderPipelineDescriptor descriptor = validDescriptor();
  descriptor.vertex.buffers[0].attributes[0].shaderLocation = kMaxVertexAttributes;
  EXPECT_THAT(device_.createRenderPipeline(descriptor),
              IsGpuErrorWithMessage(GpuErrorType::LimitExceeded, HasSubstr("shaderLocation 16")));
}

TEST_F(RenderPipelineValidationTests, RejectsMultisample) {
  RenderPipelineDescriptor descriptor = validDescriptor();
  descriptor.multisampleCount = 4;
  EXPECT_THAT(device_.createRenderPipeline(descriptor), IsGpuError(GpuErrorType::Unsupported));
}

TEST_F(RenderPipelineValidationTests, RejectsStaleLayout) {
  EXPECT_THAT(device_.destroyPipelineLayout(layout_), IsOk());
  EXPECT_THAT(device_.createRenderPipeline(validDescriptor()),
              IsGpuError(GpuErrorType::InvalidHandle));
}

// == writeBuffer ==============================================================================

TEST_F(DeviceValidationTests, WriteBufferAcceptsInBoundsWrite) {
  const Buffer buffer = createUniformBuffer(/*byteSize=*/16);
  EXPECT_THAT(device_.writeBuffer(buffer, 8, MakeBytes(8)), IsOk());
}

TEST_F(DeviceValidationTests, WriteBufferRejectsOutOfBoundsWrite) {
  const Buffer buffer = createUniformBuffer(/*byteSize=*/16);
  EXPECT_THAT(device_.writeBuffer(buffer, 8, MakeBytes(16)), IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(DeviceValidationTests, WriteBufferRejectsOffsetOverflow) {
  const Buffer buffer = createUniformBuffer(/*byteSize=*/16);
  EXPECT_THAT(device_.writeBuffer(buffer, UINT64_MAX, MakeBytes(8)),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(DeviceValidationTests, WriteBufferRejectsMissingCopyDstUsage) {
  const Buffer buffer =
      GetResultOrFail(device_.createBuffer(BufferDescriptor{"vertices", 16, BufferUsage::Vertex}));
  EXPECT_THAT(device_.writeBuffer(buffer, 0, MakeBytes(8)),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("CopyDst")));
}

// == writeTexture =============================================================================

class WriteTextureTests : public DeviceValidationTests {
protected:
  void SetUp() override {
    texture_ = GetResultOrFail(
        device_.createTexture(TextureDescriptor{"image", Extent2d{4, 4}, TextureFormat::RGBA8Unorm,
                                                TextureUsage::Sampled | TextureUsage::CopyDst}));
  }

  /// 4x4 RGBA rows padded to the 256-byte row pitch: 3 * 256 + 16 bytes.
  static constexpr size_t kPaddedByteCount = 3 * 256 + 16;

  Texture texture_;
};

TEST_F(WriteTextureTests, AcceptsAlignedFullWrite) {
  EXPECT_THAT(device_.writeTexture(texture_, MakeBytes(kPaddedByteCount),
                                   TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsOk());
}

TEST_F(WriteTextureTests, RejectsMisalignedBytesPerRow) {
  EXPECT_THAT(
      device_.writeTexture(texture_, MakeBytes(kPaddedByteCount), TexelCopyBufferLayout{0, 128, 4},
                           Extent2d{4, 4}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("not a multiple of 256")));
}

TEST_F(WriteTextureTests, RejectsBytesPerRowSmallerThanRow) {
  const Texture wide = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "wide", Extent2d{128, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  // One row of 128 RGBA texels is 512 bytes; 256 is aligned but does not cover it.
  EXPECT_THAT(
      device_.writeTexture(wide, MakeBytes(1024), TexelCopyBufferLayout{0, 256, 2},
                           Extent2d{128, 2}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("does not cover one row")));
}

TEST_F(WriteTextureTests, RejectsOffsetNotAlignedToTexelSize) {
  // RGBA8 texels are 4 bytes; an offset of 2 is not texel-aligned.
  EXPECT_THAT(device_.writeTexture(texture_, MakeBytes(kPaddedByteCount + 2),
                                   TexelCopyBufferLayout{2, 256, 4}, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("not aligned to the 4-byte texel size")));
}

TEST_F(WriteTextureTests, RejectsDataTooSmall) {
  EXPECT_THAT(device_.writeTexture(texture_, MakeBytes(kPaddedByteCount - 1),
                                   TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(WriteTextureTests, RejectsWriteSizeBeyondTexture) {
  EXPECT_THAT(device_.writeTexture(texture_, MakeBytes(8 * 256), TexelCopyBufferLayout{0, 256, 8},
                                   Extent2d{4, 8}),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(WriteTextureTests, RejectsRowsPerImageBelowHeight) {
  EXPECT_THAT(device_.writeTexture(texture_, MakeBytes(kPaddedByteCount),
                                   TexelCopyBufferLayout{0, 256, 2}, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("rowsPerImage 2")));
}

TEST_F(WriteTextureTests, RejectsMissingCopyDstUsage) {
  const Texture sampledOnly = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "sampledOnly", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
  EXPECT_THAT(device_.writeTexture(sampledOnly, MakeBytes(kPaddedByteCount),
                                   TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("CopyDst")));
}

}  // namespace
}  // namespace donner::gpu
