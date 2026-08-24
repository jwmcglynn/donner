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

TEST_F(DeviceValidationTests, CreateTextureRejectsUnknownFormat) {
  // An out-of-range format enum must fail closed instead of flowing into copy-size math
  // (TextureFormatBytesPerTexel returns 0 for unknown formats).
  EXPECT_THAT(device_.createTexture(TextureDescriptor{
                  "bogus", Extent2d{4, 4}, static_cast<TextureFormat>(255), TextureUsage::Sampled}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(DeviceValidationTests, RejectsUnknownUsageBits) {
  EXPECT_THAT(device_.createBuffer(
                  BufferDescriptor{"bogus", 16, static_cast<BufferUsage>((1u << 30) | 1u)}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
  EXPECT_THAT(
      device_.createTexture(TextureDescriptor{"bogus", Extent2d{4, 4}, TextureFormat::RGBA8Unorm,
                                              static_cast<TextureUsage>(1u << 29)}),
      IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(DeviceValidationTests, CreateSamplerRejectsUnknownEnums) {
  EXPECT_THAT(device_.createSampler(SamplerDescriptor{"bogus", static_cast<FilterMode>(7),
                                                      FilterMode::Nearest, AddressMode::ClampToEdge,
                                                      AddressMode::ClampToEdge}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
  EXPECT_THAT(device_.createSampler(
                  SamplerDescriptor{"bogus", FilterMode::Nearest, FilterMode::Nearest,
                                    static_cast<AddressMode>(9), AddressMode::ClampToEdge}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(DeviceValidationTests, CreateBindGroupLayoutRejectsUnknownEnums) {
  EXPECT_THAT(
      device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "bogus", {BindGroupLayoutEntry{0, ShaderStage::Vertex, static_cast<BindingType>(200)}}}),
      IsGpuError(GpuErrorType::InvalidDescriptor));
  EXPECT_THAT(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
                  "bogus",
                  {BindGroupLayoutEntry{0, static_cast<ShaderStage>(1u << 20),
                                        BindingType::UniformBuffer}}}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(DeviceValidationTests, CreateShaderModuleRejectsUnknownSourceKind) {
  EXPECT_THAT(device_.createShaderModule(ShaderModuleDescriptor{"bogus", "@vertex fn vsMain() {}",
                                                                static_cast<ShaderSourceKind>(9)}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
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
  BindGroupLayout layout = GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
      "uniforms", {BindGroupLayoutEntry{0, ShaderStage::Vertex, BindingType::UniformBuffer}}}));
  const BindGroupLayoutRef staleRef(layout);
  EXPECT_THAT(device_.destroyBindGroupLayout(std::move(layout)), IsOk());
  EXPECT_THAT(device_.createPipelineLayout(PipelineLayoutDescriptor{"stale", {staleRef}}),
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

TEST_F(DeviceValidationTests, CreateShaderModuleAcceptsSpirvWords) {
  EXPECT_THAT(device_.createShaderModule(ShaderModuleDescriptor{
                  "spirv", "", ShaderSourceKind::Spirv, {0x07230203u, 0x00010300u}}),
              HasResult());
}

TEST_F(DeviceValidationTests, CreateShaderModuleRejectsSpirvKindWithoutWords) {
  EXPECT_THAT(
      device_.createShaderModule(ShaderModuleDescriptor{"empty", "", ShaderSourceKind::Spirv}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("spirvWords is empty")));
}

TEST_F(DeviceValidationTests, CreateShaderModuleRejectsSpirvKindWithSourceText) {
  EXPECT_THAT(device_.createShaderModule(ShaderModuleDescriptor{
                  "both", "@vertex fn vsMain() {}", ShaderSourceKind::Spirv, {0x07230203u}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("sourceText must be empty for sourceKind Spirv")));
}

TEST_F(DeviceValidationTests, CreateShaderModuleRejectsTextKindWithSpirvWords) {
  EXPECT_THAT(device_.createShaderModule(ShaderModuleDescriptor{
                  "both", "@vertex fn vsMain() {}", ShaderSourceKind::Wgsl, {0x07230203u}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("spirvWords must be empty for text source kinds")));
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
                  "group", uniformLayout_, {BindGroupEntry{0, BufferBinding{uniform, 0, 32}}}}),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(BindGroupValidationTests, AcceptsAlignedNonzeroBufferOffset) {
  const Buffer uniform = createUniformBuffer(/*byteSize=*/kBindingOffsetAlignment + 16);
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group",
                  uniformLayout_,
                  {BindGroupEntry{0, BufferBinding{uniform, kBindingOffsetAlignment, 16}}}}),
              HasResult());
}

TEST_F(BindGroupValidationTests, RejectsMisalignedUniformBufferOffset) {
  const Buffer uniform = createUniformBuffer(/*byteSize=*/64);
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", uniformLayout_, {BindGroupEntry{0, BufferBinding{uniform, 8, 16}}}}),
              IsGpuErrorWithMessage(
                  GpuErrorType::InvalidDescriptor,
                  HasSubstr("offsetBytes 8 is not a multiple of the 256-byte binding offset "
                            "alignment")));
}

TEST_F(BindGroupValidationTests, RejectsMisalignedStorageBufferOffset) {
  const BindGroupLayout storageLayout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "storage",
          {BindGroupLayoutEntry{0, ShaderStage::Vertex, BindingType::ReadOnlyStorageBuffer}}}));
  const Buffer storage =
      GetResultOrFail(device_.createBuffer(BufferDescriptor{"storage", 64, BufferUsage::Storage}));
  EXPECT_THAT(device_.createBindGroup(BindGroupDescriptor{
                  "group", storageLayout, {BindGroupEntry{0, BufferBinding{storage, 4, 16}}}}),
              IsGpuErrorWithMessage(
                  GpuErrorType::InvalidDescriptor,
                  HasSubstr("offsetBytes 4 is not a multiple of the 256-byte binding offset "
                            "alignment")));
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

TEST_F(RenderPipelineValidationTests, RejectsUnknownEnums) {
  {
    RenderPipelineDescriptor descriptor = validDescriptor();
    descriptor.vertex.buffers[0].attributes[0].format = static_cast<VertexFormat>(77);
    EXPECT_THAT(device_.createRenderPipeline(descriptor),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
  {
    RenderPipelineDescriptor descriptor = validDescriptor();
    descriptor.fragment.targets[0].format = static_cast<TextureFormat>(255);
    EXPECT_THAT(device_.createRenderPipeline(descriptor),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
  {
    RenderPipelineDescriptor descriptor = validDescriptor();
    descriptor.fragment.targets[0].blend = BlendState{
        BlendComponent{static_cast<BlendFactor>(99), BlendFactor::One, BlendOperation::Add},
        BlendComponent{}};
    EXPECT_THAT(device_.createRenderPipeline(descriptor),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
  {
    RenderPipelineDescriptor descriptor = validDescriptor();
    descriptor.fragment.targets[0].writeMask = static_cast<ColorWriteMask>(1u << 8);
    EXPECT_THAT(device_.createRenderPipeline(descriptor),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
  {
    RenderPipelineDescriptor descriptor = validDescriptor();
    descriptor.topology = static_cast<PrimitiveTopology>(9);
    EXPECT_THAT(device_.createRenderPipeline(descriptor),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
  {
    RenderPipelineDescriptor descriptor = validDescriptor();
    descriptor.cullMode = static_cast<CullMode>(9);
    EXPECT_THAT(device_.createRenderPipeline(descriptor),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
  {
    RenderPipelineDescriptor descriptor = validDescriptor();
    descriptor.vertex.buffers[0].stepMode = static_cast<VertexStepMode>(9);
    EXPECT_THAT(device_.createRenderPipeline(descriptor),
                IsGpuError(GpuErrorType::InvalidDescriptor));
  }
}

TEST_F(RenderPipelineValidationTests, RejectsMultisample) {
  RenderPipelineDescriptor descriptor = validDescriptor();
  descriptor.multisampleCount = 4;
  EXPECT_THAT(device_.createRenderPipeline(descriptor), IsGpuError(GpuErrorType::Unsupported));
}

TEST_F(RenderPipelineValidationTests, RejectsStaleLayout) {
  // Build the descriptor first so it holds a genuinely stale layout reference.
  const RenderPipelineDescriptor descriptor = validDescriptor();
  EXPECT_THAT(device_.destroyPipelineLayout(std::move(layout_)), IsOk());
  EXPECT_THAT(device_.createRenderPipeline(descriptor), IsGpuError(GpuErrorType::InvalidHandle));
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

// ----------------------------------------------------------------------------
// Host buffer mapping.

/// A device whose mapping and presentation hooks follow a script, so a test can state exactly
/// what the backend reports and assert what the runtime does with it.
class ScriptedDevice : public Device {
public:
  /// Submissions complete instantly here; nothing in these tests waits on one.
  uint64_t completedSerial() const override { return lastSubmittedSerial(); }

  /// What every slice reports until \ref sliceStates runs out.
  MapSliceState trailingState = MapSliceState::Pending;
  /// States reported by the first slices, in order.
  std::vector<MapSliceState> sliceStates;
  /// Number of slices the runtime asked for.
  int sliceCalls = 0;
  /// Bytes handed back once a mapping is ready.
  std::vector<uint8_t> bytes{1, 2, 3, 4};
  /// Number of times the runtime released a mapping.
  int unmapCalls = 0;
  /// What acquiring reports.
  SurfaceStatus acquireStatus = SurfaceStatus::Success;
  /// What presenting reports.
  SurfaceStatus presentStatus = SurfaceStatus::Success;
  /// Number of times the runtime abandoned an acquired texture.
  int abandonCalls = 0;

protected:
  // The operations this fake does not model are accepted and ignored: the tests below are about
  // mapping, and a backend that recorded them would only add noise.
  Status onCreateBuffer(uint32_t, const BufferDescriptor&) override { return OkStatus(); }
  Status onCreateTexture(uint32_t, const TextureDescriptor&) override { return OkStatus(); }
  Status onCreateTextureView(uint32_t, uint32_t, const TextureViewDescriptor&) override {
    return OkStatus();
  }
  Status onCreateSampler(uint32_t, const SamplerDescriptor&) override { return OkStatus(); }
  Status onCreateBindGroupLayout(uint32_t, const BindGroupLayoutDescriptor&) override {
    return OkStatus();
  }
  Status onCreateBindGroup(uint32_t, const BindGroupDescriptor&) override { return OkStatus(); }
  Status onCreatePipelineLayout(uint32_t, const PipelineLayoutDescriptor&) override {
    return OkStatus();
  }
  Status onCreateShaderModule(uint32_t, const ShaderModuleDescriptor&) override {
    return OkStatus();
  }
  Status onCreateRenderPipeline(uint32_t, const RenderPipelineDescriptor&) override {
    return OkStatus();
  }
  Status onCreateComputePipeline(uint32_t, const ComputePipelineDescriptor&) override {
    return OkStatus();
  }
  void onDestroyResource(std::string_view, uint32_t) override {}
  Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override { return OkStatus(); }
  Status onWriteTexture(uint32_t, std::span<const uint8_t>, const TexelCopyBufferLayout&,
                        const Extent2d&) override {
    return OkStatus();
  }
  Status onSubmit(uint64_t, uint32_t, std::span<const Command>) override { return OkStatus(); }

  Status onCreateSurface(uint32_t, const SurfaceDescriptor&) override { return OkStatus(); }

  Result<SurfaceCapabilities> onSurfaceCapabilities(uint32_t) const override {
    return SurfaceCapabilities{{TextureFormat::BGRA8Unorm},
                               TextureUsage::RenderAttachment,
                               {PresentMode::Fifo},
                               {SurfaceAlphaMode::Opaque}};
  }

  Status onConfigureSurface(uint32_t, const SurfaceConfiguration&) override { return OkStatus(); }

  Result<SurfaceStatus> onAcquireCurrentTexture(uint32_t, uint32_t) override {
    return acquireStatus;
  }

  Result<SurfaceStatus> onPresentSurface(uint32_t) override { return presentStatus; }

  void onAbandonCurrentTexture(uint32_t) override { ++abandonCalls; }

  Status onMapBufferAsync(uint32_t /*mappingSlotIndex*/, uint32_t /*bufferSlotIndex*/,
                          MapMode /*mode*/, uint64_t /*offsetBytes*/,
                          uint64_t /*byteCount*/) override {
    return OkStatus();
  }

  MapSliceState onWaitMappingSlice(uint32_t /*mappingSlotIndex*/,
                                   double /*sliceSeconds*/) override {
    const size_t index = static_cast<size_t>(sliceCalls);
    ++sliceCalls;
    return index < sliceStates.size() ? sliceStates[index] : trailingState;
  }

  Result<std::span<const uint8_t>> onMappedBytes(uint32_t /*mappingSlotIndex*/) const override {
    return std::span<const uint8_t>(bytes);
  }

  void onUnmapBuffer(uint32_t /*mappingSlotIndex*/) override { ++unmapCalls; }
};

class BufferMappingTests : public testing::Test {
protected:
  Buffer readableBuffer(uint64_t byteSize = 64) {
    return GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"readback", byteSize, BufferUsage::CopyDst | BufferUsage::MapRead}));
  }

  /// A wait with slices short enough that the budget allows exactly four of them.
  static MapWaitParams fourSlices() { return MapWaitParams{0.25, 1.0}; }

  ScriptedDevice device_;
};

TEST_F(BufferMappingTests, RejectsBufferWithoutMapReadUsage) {
  const Buffer plain =
      GetResultOrFail(device_.createBuffer(BufferDescriptor{"plain", 64, BufferUsage::CopyDst}));
  EXPECT_THAT(device_.mapBufferAsync(plain, MapMode::Read, 0, 64),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("lacks the MapRead")));
}

TEST_F(BufferMappingTests, RejectsRangeBeyondTheBuffer) {
  const Buffer buffer = readableBuffer();
  EXPECT_THAT(device_.mapBufferAsync(buffer, MapMode::Read, 32, 64),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(BufferMappingTests, RejectsEmptyRange) {
  const Buffer buffer = readableBuffer();
  EXPECT_THAT(device_.mapBufferAsync(buffer, MapMode::Read, 0, 0),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(BufferMappingTests, RejectsWaitBoundsThatCouldNeverTerminate) {
  const Buffer buffer = readableBuffer();
  BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));
  EXPECT_THAT(device_.waitForMapping(mapping, MapWaitParams{0.0, 1.0}, {}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
  EXPECT_THAT(device_.waitForMapping(mapping, MapWaitParams{0.25, 0.0}, {}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(BufferMappingTests, ReadyMappingHandsBackItsBytes) {
  const Buffer buffer = readableBuffer();
  device_.sliceStates = {MapSliceState::Pending, MapSliceState::Ready};
  BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));

  EXPECT_EQ(GetResultOrFail(device_.waitForMapping(mapping, fourSlices(), {})),
            MapWaitOutcome::Ready);
  EXPECT_THAT(GetResultOrFail(device_.mappedBytes(mapping)), testing::ElementsAre(1, 2, 3, 4));
}

TEST_F(BufferMappingTests, CancellationIsCheckedBeforeAnySliceRuns) {
  const Buffer buffer = readableBuffer();
  BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));

  EXPECT_EQ(GetResultOrFail(device_.waitForMapping(mapping, fourSlices(), [] { return true; })),
            MapWaitOutcome::Cancelled);
  EXPECT_EQ(device_.sliceCalls, 0)
      << "A caller that has already cancelled must not be made to wait a slice first";
}

TEST_F(BufferMappingTests, CancellationStopsWithinOneSlice) {
  const Buffer buffer = readableBuffer();
  BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));

  int checks = 0;
  const auto cancelOnSecondCheck = [&checks] { return ++checks > 1; };
  EXPECT_EQ(GetResultOrFail(device_.waitForMapping(mapping, fourSlices(), cancelOnSecondCheck)),
            MapWaitOutcome::Cancelled);
  EXPECT_EQ(device_.sliceCalls, 1) << "Cancellation must end the wait after the slice in flight";
}

TEST_F(BufferMappingTests, BudgetExhaustionReportsTimedOut) {
  const Buffer buffer = readableBuffer();
  BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));

  EXPECT_EQ(GetResultOrFail(device_.waitForMapping(mapping, fourSlices(), {})),
            MapWaitOutcome::TimedOut);
  EXPECT_EQ(device_.sliceCalls, 4) << "The budget must be spent in slices of the stated length";
}

TEST_F(BufferMappingTests, DeviceLossEndsTheWaitImmediately) {
  const Buffer buffer = readableBuffer();
  device_.sliceStates = {MapSliceState::DeviceLost};
  BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));

  EXPECT_EQ(GetResultOrFail(device_.waitForMapping(mapping, fourSlices(), {})),
            MapWaitOutcome::DeviceLost);
  EXPECT_EQ(device_.sliceCalls, 1)
      << "A lost device can never deliver the map, so the budget must not be spent on it";
}

TEST_F(BufferMappingTests, BackendFailureIsItsOwnOutcome) {
  const Buffer buffer = readableBuffer();
  device_.sliceStates = {MapSliceState::Failed};
  BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));

  EXPECT_EQ(GetResultOrFail(device_.waitForMapping(mapping, fourSlices(), {})),
            MapWaitOutcome::Failed);
}

TEST_F(BufferMappingTests, ReleasingAMappingInvalidatesItsHandle) {
  const Buffer buffer = readableBuffer();
  device_.trailingState = MapSliceState::Ready;
  BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));
  ASSERT_EQ(GetResultOrFail(device_.waitForMapping(mapping, fourSlices(), {})),
            MapWaitOutcome::Ready);

  EXPECT_THAT(device_.unmapBuffer(std::move(mapping)), IsOk());
  EXPECT_EQ(device_.unmapCalls, 1);
  EXPECT_THAT(device_.mappedBytes(mapping), IsGpuError(GpuErrorType::InvalidHandle))
      << "Reading through a released mapping must be reported, not attempted";
  EXPECT_THAT(device_.waitForMapping(mapping, fourSlices(), {}),
              IsGpuError(GpuErrorType::InvalidHandle));
}

TEST_F(BufferMappingTests, DroppingAMappingReleasesItExactlyOnce) {
  const Buffer buffer = readableBuffer();
  {
    BufferMapping mapping = GetResultOrFail(device_.mapBufferAsync(buffer, MapMode::Read, 0, 64));
    EXPECT_EQ(device_.unmapCalls, 0);
  }
  EXPECT_EQ(device_.unmapCalls, 1) << "A dropped mapping must release through the same path";
}

TEST_F(BufferMappingTests, ABackendWithoutMappingReportsItUnsupported) {
  RecordingDevice plainDevice;
  const Buffer buffer = GetResultOrFail(plainDevice.createBuffer(
      BufferDescriptor{"readback", 64, BufferUsage::CopyDst | BufferUsage::MapRead}));
  EXPECT_THAT(plainDevice.mapBufferAsync(buffer, MapMode::Read, 0, 64),
              IsGpuError(GpuErrorType::Unsupported));
}

// ----------------------------------------------------------------------------
// Surface presentation.

class SurfaceTests : public testing::Test {
protected:
  Surface metalSurface() {
    SurfaceDescriptor descriptor;
    descriptor.label = "window";
    descriptor.native.kind = NativeSurfaceKind::MetalLayer;
    descriptor.native.display = &layer_;
    return GetResultOrFail(device_.createSurface(descriptor));
  }

  static SurfaceConfiguration configuration(uint32_t width = 640, uint32_t height = 480) {
    return SurfaceConfiguration{TextureFormat::BGRA8Unorm, TextureUsage::RenderAttachment,
                                Extent2d{width, height}, PresentMode::Fifo,
                                SurfaceAlphaMode::Opaque};
  }

  int layer_ = 0;
  ScriptedDevice device_;
};

TEST_F(SurfaceTests, RejectsANativeHandleMissingItsPayload) {
  SurfaceDescriptor withoutLayer;
  withoutLayer.native.kind = NativeSurfaceKind::MetalLayer;
  EXPECT_THAT(device_.createSurface(withoutLayer), IsGpuError(GpuErrorType::InvalidDescriptor));

  SurfaceDescriptor withoutSelector;
  withoutSelector.native.kind = NativeSurfaceKind::CanvasSelector;
  EXPECT_THAT(device_.createSurface(withoutSelector),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("selector")));
}

TEST_F(SurfaceTests, RejectsAPayloadSlotItsKindDoesNotUse) {
  SurfaceDescriptor canvasWithWindow;
  canvasWithWindow.native.kind = NativeSurfaceKind::CanvasSelector;
  canvasWithWindow.native.selector = "#canvas";
  canvasWithWindow.native.window = 7;
  EXPECT_THAT(device_.createSurface(canvasWithWindow),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("does not use a window handle")));

  SurfaceDescriptor layerWithSelector;
  layerWithSelector.native.kind = NativeSurfaceKind::MetalLayer;
  layerWithSelector.native.display = &layer_;
  layerWithSelector.native.selector = "#canvas";
  EXPECT_THAT(
      device_.createSurface(layerWithSelector),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("does not use a selector")));

  SurfaceDescriptor windowKindWithoutWindow;
  windowKindWithoutWindow.native.kind = NativeSurfaceKind::XlibWindow;
  windowKindWithoutWindow.native.display = &layer_;
  EXPECT_THAT(
      device_.createSurface(windowKindWithoutWindow),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("needs a window handle")));
}

TEST_F(SurfaceTests, AcceptsACanvasNamedBySelector) {
  SurfaceDescriptor descriptor;
  descriptor.native.kind = NativeSurfaceKind::CanvasSelector;
  descriptor.native.selector = "#canvas";
  EXPECT_THAT(device_.createSurface(descriptor), IsOk());
}

TEST_F(SurfaceTests, RejectsAConfigurationWithNoExtent) {
  const Surface surface = metalSurface();
  EXPECT_THAT(device_.configureSurface(surface, configuration(0, 480)),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(SurfaceTests, AcquiringBeforeConfiguringIsReported) {
  const Surface surface = metalSurface();
  EXPECT_THAT(device_.acquireCurrentTexture(surface),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("not been configured")));
}

TEST_F(SurfaceTests, AcquiringTwiceWithoutResolvingTheFirstIsReported) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());
  SurfaceTexture first = GetResultOrFail(device_.acquireCurrentTexture(surface));
  ASSERT_TRUE(first.texture.isValid());

  EXPECT_THAT(device_.acquireCurrentTexture(surface), IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(SurfaceTests, DisposingTheAcquiredTextureLeavesNoFrameOutstanding) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());
  {
    SurfaceTexture frame = GetResultOrFail(device_.acquireCurrentTexture(surface));
    ASSERT_TRUE(frame.texture.isValid());
    // Dropping the handle destroys the texture through the device, which is how every other
    // resource of this runtime is disposed of.
  }

  EXPECT_THAT(device_.acquireCurrentTexture(surface), IsOk())
      << "A surface whose acquired texture is already gone has no frame left to resolve";
}

TEST_F(SurfaceTests, PresentingInvalidatesTheAcquiredTexture) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());
  SurfaceTexture frame = GetResultOrFail(device_.acquireCurrentTexture(surface));

  EXPECT_EQ(GetResultOrFail(device_.presentSurface(surface)), SurfaceStatus::Success);
  EXPECT_THAT(device_.createTextureView(frame.texture, TextureViewDescriptor{"view"}),
              IsGpuError(GpuErrorType::InvalidHandle))
      << "The platform owns the texture once it has been presented";
}

TEST_F(SurfaceTests, AbandoningInvalidatesTheAcquiredTexture) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());
  SurfaceTexture frame = GetResultOrFail(device_.acquireCurrentTexture(surface));

  EXPECT_THAT(device_.abandonCurrentTexture(surface), IsOk());
  EXPECT_EQ(device_.abandonCalls, 1);
  EXPECT_THAT(device_.createTextureView(frame.texture, TextureViewDescriptor{"view"}),
              IsGpuError(GpuErrorType::InvalidHandle));
}

TEST_F(SurfaceTests, ReconfiguringInvalidatesTheAcquiredTexture) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());
  SurfaceTexture frame = GetResultOrFail(device_.acquireCurrentTexture(surface));

  // A resize is a reconfiguration of the same surface, not a new one.
  EXPECT_THAT(device_.configureSurface(surface, configuration(800, 600)), IsOk());
  EXPECT_THAT(device_.createTextureView(frame.texture, TextureViewDescriptor{"view"}),
              IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.acquireCurrentTexture(surface), IsOk())
      << "A reconfigured surface hands out frames again without being recreated";
}

TEST_F(SurfaceTests, PresentingWithoutAcquiringIsReported) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());
  EXPECT_THAT(device_.presentSurface(surface), IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(SurfaceTests, AnOutdatedSurfaceStillHandsBackAUsableFrame) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());
  device_.acquireStatus = SurfaceStatus::Outdated;

  SurfaceTexture frame = GetResultOrFail(device_.acquireCurrentTexture(surface));
  EXPECT_EQ(frame.status, SurfaceStatus::Outdated);
  EXPECT_TRUE(frame.texture.isValid())
      << "A surface that has drifted out of date usually still presents, so the caller chooses";
}

TEST_F(SurfaceTests, ALostSurfaceHandsBackNoFrame) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());

  for (const SurfaceStatus status :
       {SurfaceStatus::Lost, SurfaceStatus::DeviceLost, SurfaceStatus::Timeout}) {
    device_.acquireStatus = status;
    SurfaceTexture frame = GetResultOrFail(device_.acquireCurrentTexture(surface));
    EXPECT_EQ(frame.status, status);
    EXPECT_FALSE(frame.texture.isValid());
  }
}

TEST_F(SurfaceTests, PresentReportsWhatTheSurfaceSaid) {
  const Surface surface = metalSurface();
  ASSERT_THAT(device_.configureSurface(surface, configuration()), IsOk());
  device_.presentStatus = SurfaceStatus::Lost;
  (void)GetResultOrFail(device_.acquireCurrentTexture(surface));

  EXPECT_EQ(GetResultOrFail(device_.presentSurface(surface)), SurfaceStatus::Lost);
}

TEST_F(SurfaceTests, CapabilitiesComeFromTheSurface) {
  const Surface surface = metalSurface();
  const SurfaceCapabilities caps = GetResultOrFail(device_.surfaceCapabilities(surface));
  EXPECT_THAT(caps.formats, testing::ElementsAre(TextureFormat::BGRA8Unorm));
  EXPECT_THAT(caps.presentModes, testing::ElementsAre(PresentMode::Fifo));
}

TEST_F(SurfaceTests, ABackendWithoutPresentationReportsItUnsupported) {
  RecordingDevice plainDevice;
  SurfaceDescriptor descriptor;
  descriptor.native.kind = NativeSurfaceKind::MetalLayer;
  descriptor.native.display = &layer_;
  EXPECT_THAT(plainDevice.createSurface(descriptor), IsGpuError(GpuErrorType::Unsupported));
}

}  // namespace
}  // namespace donner::gpu
