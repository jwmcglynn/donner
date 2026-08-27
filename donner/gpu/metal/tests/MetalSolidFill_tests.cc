/// @file
/// The Metal solid-fill vertical slice: renders the shared baseline scene
/// through donner::gpu::metal::MetalDevice with the MSL emitted from the solid-fill IR program,
/// and compares pixels against the frozen baseline captured from the current production
/// renderer.
///
/// The geometry, uniforms, and draw sequence follow the production encoder's fillPath data flow:
/// GeodePathEncoder banding, the same clip-space MVP construction (pixel -> clip with the Y flip
/// for a top-left origin), premultiplied colors, an identity per-instance transform at binding 7,
/// and 1x1 dummy pattern/clip textures at bindings 3..6. The compact canonical curve references
/// are expanded for the shader IR's current contiguous-curve input layout, and the conservative
/// path bounds are expressed through its current vertex-buffer quad contract.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/baseline/FrozenBaselinePolicy.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/shader/tests/StageIoTestModules.h"
#include "donner/gpu/tests/BaselineScene.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"

using testing::HasSubstr;

namespace donner::gpu::metal::tests {
namespace {

using geode::EncodedPath;
using gpu::tests::BaselinePathSpec;
using gpu::tests::BaselinePixelFromScene;
using gpu::tests::BaselineScenePaths;
using gpu::tests::BuildIdentity4x4;
using gpu::tests::BuildSolidFillMvp;
using gpu::tests::ExpandLegacyAxis;
using gpu::tests::kBaselineSize;
using gpu::tests::LegacyAxis;
using gpu::tests::LegacyBand;
using gpu::tests::SolidFillUniforms;
using gpu::tests::WriteBoundingPolygon;

constexpr uint32_t kBytesPerRow = kBaselineSize * 4;  // 1024; already 256-byte aligned.

/// C++ mirror of the shader's 288-byte Uniforms struct (layout anchored by the shader IR layout
/// tests; field order matches slug_fill/the solid-fill IR program).

/// A storage buffer plus the byte size it was created with, so bind groups can bind the FULL
/// range honestly. Vulkan enforces VkDescriptorBufferInfo.range, so binding a smaller range than
/// the shader indexes would read out of bounds; Metal ignores the range, but both slices bind the
/// same honest sizes.
struct SizedBuffer {
  Buffer buffer;           //!< The storage buffer.
  uint64_t sizeBytes = 0;  //!< Byte size the buffer was created with.
};

/// Where the frozen per-adapter baselines live in the runfiles tree.
constexpr const char* kBaselinesRunfileDir = "donner/gpu/baseline/baselines";

/// One path's GPU resources.
struct PathDraw {
  Buffer uniformBuffer;      //!< The solid-fill uniform block.
  SizedBuffer bands;         //!< Horizontal bands (or one zero band).
  SizedBuffer curves;        //!< Horizontal curves (or 4-byte dummy).
  SizedBuffer vBands;        //!< Vertical bands (or one zero band).
  SizedBuffer vCurves;       //!< Vertical curves (or 4-byte dummy).
  SizedBuffer hGrid;         //!< Horizontal band grid (or 4-byte dummy).
  SizedBuffer vGrid;         //!< Vertical band grid (or 4-byte dummy).
  BindGroup bindGroup;       //!< The 12-entry solid-fill bind group.
  uint32_t vertexCount = 0;  //!< Draw vertex count.
};

class MetalSolidFillTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    if (!device_) {
      // Same rule the frozen pixel gate uses: a lane that selected this target and then created
      // no device has disabled the comparison, and reporting that as a pass hides it.
      const baseline::MissingComparisonDisposition disposition =
          baseline::DispositionForMissingAdapter(baseline::RunningUnderContinuousIntegration());
      const std::string message =
          baseline::NoAdapterMessage("the Metal solid-fill slice", disposition);
      if (disposition == baseline::MissingComparisonDisposition::FailClosed) {
        FAIL() << message;
      }
      GTEST_SKIP() << message;
    }
  }

  /**
   * Runfiles path of the frozen baseline filed for \p adapterName, or empty when no directory
   * matches.
   *
   * Two GPUs running the same shaders round a covered edge texel differently, so the frozen
   * pixels are filed one directory per adapter and this slice resolves its own. Sharing the
   * corpus rather than keeping a second copy of the same bytes is deliberate: a private golden is
   * what let this slice drift away from the renderer it exists to validate.
   *
   * The lookup deliberately does NOT live in SetUp. The baseline is an input to the pixel
   * comparison and to nothing else in this fixture; gating every case on it would let a corpus
   * that has not caught up with new hardware take unrelated regressions down with it, red on an
   * automated lane and silent locally.
   *
   * @param adapterName Adapter to resolve a baseline for.
   * @return The runfiles path, or an empty string when this adapter has no committed baseline.
   */
  static std::string baselinePathFor(std::string_view adapterName) {
    const std::string path = std::string(kBaselinesRunfileDir) + "/" +
                             baseline::AdapterSlug(adapterName, "Metal") +
                             "/solid_fill_baseline.png";
    // Rlocation composes a path without looking for the file, so the existence test has to be
    // explicit. Returning a path for an adapter with no committed directory would walk straight
    // past the unbaselined rule and fail later as a golden that would not open, which reports a
    // missing baseline as a pixel mismatch.
    std::error_code error;
    return std::filesystem::exists(Runfiles::instance().Rlocation(path), error) ? path
                                                                                : std::string();
  }

  /// Unwraps an RHI result, failing the test on error.
  template <typename T>
  T unwrap(Result<T>&& result, const char* what) {
    if (result.hasError()) {
      ADD_FAILURE() << what << " failed: " << result.error();
    }
    return std::move(result).result();
  }

  /// Creates a storage buffer holding \p bytes. Empty buffers receive a zero-filled dummy large
  /// enough for one shader element so Metal validation can prove every runtime-array binding.
  /// Returns the buffer with its created byte size so bind groups bind the full range.
  SizedBuffer storageBuffer(const char* label, const void* data, size_t byteCount,
                            size_t emptyByteCount = sizeof(uint32_t)) {
    const std::array<uint8_t, sizeof(LegacyBand)> dummy = {};
    if (byteCount == 0) {
      data = dummy.data();
      byteCount = emptyByteCount;
    }
    Buffer buffer = unwrap(device_->createBuffer(BufferDescriptor{
                               label, byteCount, BufferUsage::Storage | BufferUsage::CopyDst}),
                           label);
    const Status writeStatus = device_->writeBuffer(
        buffer, 0, std::span<const uint8_t>(static_cast<const uint8_t*>(data), byteCount));
    EXPECT_FALSE(writeStatus.hasError()) << writeStatus.error();
    return SizedBuffer{std::move(buffer), byteCount};
  }

  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalSolidFillTest, ReadBackBufferRejectsStaleHandleAfterSlotReuse) {
  // This case must run on any adapter, including one the corpus has no baseline for: it compares
  // no pixels, and gating it on the corpus would let hardware the baselines have not caught up
  // with hide a real buffer regression - red on an automated lane, silent everywhere else.
  //
  // Two things hold that. Structurally, SetUp resolves no baseline and this fixture stores none,
  // so nothing but the pixel case can be gated on one. And the unbaselined branch is reachable
  // rather than dead: an adapter name no directory can match comes back empty here, which is only
  // true because the resolver tests for the file rather than trusting a composed runfiles path.
  EXPECT_TRUE(baselinePathFor("no adapter is named this").empty());

  // The readback helper must validate the handle's generation: after destroy + recreate the
  // freed slot is reused, and a stale handle must fail closed instead of reading the wrong
  // buffer.
  Buffer original = unwrap(device_->createBuffer(BufferDescriptor{
                               "original", 16, BufferUsage::CopyDst | BufferUsage::MapRead}),
                           "createBuffer original");
  const uint32_t originalSlot = original.slotIndex();
  const uint32_t originalGeneration = original.generation();
  const uint64_t deviceId = original.deviceId();
  const Status destroyStatus = device_->destroyBuffer(std::move(original));
  ASSERT_FALSE(destroyStatus.hasError()) << destroyStatus.error();

  Buffer replacement = unwrap(device_->createBuffer(BufferDescriptor{
                                  "replacement", 16, BufferUsage::CopyDst | BufferUsage::MapRead}),
                              "createBuffer replacement");
  ASSERT_EQ(replacement.slotIndex(), originalSlot);
  ASSERT_NE(replacement.generation(), originalGeneration);

  // Forge a handle carrying the retired generation (destroy consumed the real handle), proving
  // the readback helper checks generation, not just slot.
  const Buffer staleHandle = Buffer::CreateForBackend(originalSlot, originalGeneration, deviceId);
  Result<std::vector<uint8_t>> stale = device_->readBackBuffer(staleHandle);
  ASSERT_TRUE(stale.hasError()) << "stale readback unexpectedly succeeded";
  EXPECT_EQ(stale.error().type, GpuErrorType::InvalidHandle) << stale.error();
}

TEST_F(MetalSolidFillTest, EmittedMslForAPositionOnlyFragmentEntryCompilesOnTheDevice) {
  // The offline Metal compiler ships as a downloadable Xcode component, so the out-of-process MSL
  // validation skips wherever it is absent. The runtime compiler behind createShaderModule is
  // there on any machine with a device, which makes it the one that holds the emitter's stage IO
  // shapes everywhere this slice runs.
  //
  // This entry declares no location at all: its only input is the position builtin. Each emitter
  // decides for itself how such an input reaches the stage, so it is the shape one of them can
  // get wrong while every shipped program keeps working.
  shader::ShaderResult<shader::IrModule> module = shader::BuildPositionOnlyFragmentModule();
  ASSERT_FALSE(module.hasError()) << module.error();
  shader::ShaderResult<std::string> msl = shader::EmitMsl(module.result());
  ASSERT_FALSE(msl.hasError()) << msl.error();

  Result<ShaderModule> compiled = device_->createShaderModule(ShaderModuleDescriptor{
      "positionOnlyFragment", RcString(msl.result()), ShaderSourceKind::Msl});
  EXPECT_FALSE(compiled.hasError())
      << "the device rejected the emitted MSL: " << compiled.error() << "\n"
      << msl.result();
}

TEST_F(MetalSolidFillTest, MatchesFrozenBaseline) {
  // ----- The frozen baseline for this run's adapter -----
  const std::string goldenPath = baselinePathFor(device_->adapterName());
  if (goldenPath.empty()) {
    const baseline::MissingComparisonDisposition disposition =
        baseline::DispositionForUnbaselinedAdapter(baseline::RunningUnderContinuousIntegration());
    const std::string message = baseline::UnbaselinedAdapterMessage(
        device_->adapterName(), "Metal", baseline::AdapterSlug(device_->adapterName(), "Metal"),
        /*capturedPath=*/"",
        "this slice renders through donner::gpu, not the production path the baselines come "
        "from; capture one with //donner/gpu/baseline:capture_baselines",
        disposition);
    if (disposition == baseline::MissingComparisonDisposition::FailClosed) {
      FAIL() << message;
    }
    GTEST_SKIP() << message;
  }

  // ----- Shader module and pipeline from the emitted MSL -----
  shader::ShaderResult<shader::IrModule> irModule = shader::programs::BuildSolidFillModule();
  ASSERT_FALSE(irModule.hasError()) << irModule.error();
  shader::ShaderResult<std::string> msl = shader::EmitMsl(irModule.result());
  ASSERT_FALSE(msl.hasError()) << msl.error();

  ShaderModule shaderModule =
      unwrap(device_->createShaderModule(ShaderModuleDescriptor{"solidFill", RcString(msl.result()),
                                                                ShaderSourceKind::Msl}),
             "createShaderModule");

  // The 12-entry bind group layout mirroring the production solid-fill pipeline's stage
  // visibilities: uniforms vertex+fragment, instance transforms vertex-only, everything else
  // fragment.
  std::vector<BindGroupLayoutEntry> bglEntries;
  bglEntries.push_back(
      {0, ShaderStage::Vertex | ShaderStage::Fragment, BindingType::UniformBuffer});
  bglEntries.push_back({1, ShaderStage::Fragment, BindingType::ReadOnlyStorageBuffer});
  bglEntries.push_back({2, ShaderStage::Fragment, BindingType::ReadOnlyStorageBuffer});
  bglEntries.push_back({3, ShaderStage::Fragment, BindingType::SampledTexture2dFloat});
  bglEntries.push_back({4, ShaderStage::Fragment, BindingType::FilteringSampler});
  bglEntries.push_back({5, ShaderStage::Fragment, BindingType::SampledTexture2dFloat});
  bglEntries.push_back({6, ShaderStage::Fragment, BindingType::FilteringSampler});
  bglEntries.push_back({7, ShaderStage::Vertex, BindingType::ReadOnlyStorageBuffer});
  bglEntries.push_back({8, ShaderStage::Fragment, BindingType::ReadOnlyStorageBuffer});
  bglEntries.push_back({9, ShaderStage::Fragment, BindingType::ReadOnlyStorageBuffer});
  bglEntries.push_back({10, ShaderStage::Fragment, BindingType::ReadOnlyStorageBuffer});
  bglEntries.push_back({11, ShaderStage::Fragment, BindingType::ReadOnlyStorageBuffer});
  BindGroupLayout bindGroupLayout =
      unwrap(device_->createBindGroupLayout(BindGroupLayoutDescriptor{"solidFillBGL", bglEntries}),
             "createBindGroupLayout");
  PipelineLayout pipelineLayout = unwrap(
      device_->createPipelineLayout(PipelineLayoutDescriptor{"solidFillPL", {bindGroupLayout}}),
      "createPipelineLayout");

  // No vertex buffers: the stage builds the bounding fan from vertex_index.
  RenderPipelineDescriptor pipelineDescriptor{
      "solidFill", pipelineLayout, VertexState{shaderModule, "vs_main", {}},
      FragmentState{shaderModule,
                    "fs_main",
                    {ColorTargetState{
                        TextureFormat::RGBA8Unorm,
                        BlendState{BlendComponent{BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                                                  BlendOperation::Add},
                                   BlendComponent{BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                                                  BlendOperation::Add}}}}}};
  RenderPipeline pipeline =
      unwrap(device_->createRenderPipeline(pipelineDescriptor), "createRenderPipeline");

  // ----- Render target, readback, dummies, identity instance transform -----
  Texture target =
      unwrap(device_->createTexture(TextureDescriptor{
                 "target", Extent2d{kBaselineSize, kBaselineSize}, TextureFormat::RGBA8Unorm,
                 TextureUsage::RenderAttachment | TextureUsage::CopySrc}),
             "createTexture target");
  TextureView targetView = unwrap(
      device_->createTextureView(target, TextureViewDescriptor{"targetView"}), "createTextureView");
  Buffer readback =
      unwrap(device_->createBuffer(BufferDescriptor{"readback", kBytesPerRow * kBaselineSize,
                                                    BufferUsage::CopyDst | BufferUsage::MapRead}),
             "createBuffer readback");

  // 1x1 transparent dummy pattern/clip textures + samplers (mirroring the production device's
  // always-bound dummies).
  Texture dummyTexture = unwrap(
      device_->createTexture(TextureDescriptor{"dummy", Extent2d{1, 1}, TextureFormat::RGBA8Unorm,
                                               TextureUsage::Sampled | TextureUsage::CopyDst}),
      "createTexture dummy");
  // Sized to the row pitch the layout declares, not to the single texel: a backend may copy
  // whole strided rows out of the source, so a four-byte array would be read past its end.
  const std::array<uint8_t, 256> dummyTexel = {};
  const Status dummyWrite = device_->writeTexture(dummyTexture, dummyTexel,
                                                  TexelCopyBufferLayout{0, 256, 1}, Extent2d{1, 1});
  ASSERT_FALSE(dummyWrite.hasError()) << dummyWrite.error();
  TextureView dummyView =
      unwrap(device_->createTextureView(dummyTexture, TextureViewDescriptor{"dummyView"}),
             "createTextureView dummy");
  Sampler dummySampler = unwrap(device_->createSampler(SamplerDescriptor{
                                    "dummySampler", FilterMode::Linear, FilterMode::Linear}),
                                "createSampler");

  struct IdentityInstanceTransform {
    float row0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float row1[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  } identityTransform;
  SizedBuffer instanceTransforms =
      storageBuffer("instanceTransforms", &identityTransform, sizeof(identityTransform));

  // ----- Per-path geometry, uniforms, and bind groups (the production fillPath data flow) ----
  const Transform2d pixelFromScene = BaselinePixelFromScene();
  std::vector<PathDraw> draws;
  for (const BaselinePathSpec& spec : BaselineScenePaths()) {
    const EncodedPath encoded = geode::GeodePathEncoder::encode(spec.path, spec.rule);
    ASSERT_GE(encoded.boundingVertexCount, 3u);
    LegacyAxis horizontal;
    LegacyAxis vertical;
    ASSERT_TRUE(ExpandLegacyAxis(encoded.bands, encoded.curveIndices, encoded.curves, horizontal));
    ASSERT_TRUE(ExpandLegacyAxis(encoded.vBands, encoded.vCurveIndices, encoded.vCurves, vertical));

    PathDraw draw;
    // The vertex shader expands the bounding fan from vertex_index; there is no vertex buffer.
    draw.vertexCount = encoded.boundingDrawVertexCount();

    draw.bands = storageBuffer("bands", horizontal.bands.data(),
                               horizontal.bands.size() * sizeof(LegacyBand), sizeof(LegacyBand));
    draw.curves = storageBuffer("curves", horizontal.curves.data(),
                                horizontal.curves.size() * sizeof(EncodedPath::Curve));
    draw.vBands = storageBuffer("vBands", vertical.bands.data(),
                                vertical.bands.size() * sizeof(LegacyBand), sizeof(LegacyBand));
    draw.vCurves = storageBuffer("vCurves", vertical.curves.data(),
                                 vertical.curves.size() * sizeof(EncodedPath::Curve));
    draw.hGrid = storageBuffer("hBandGrid", encoded.hBandGrid.data(),
                               encoded.hBandGrid.size() * sizeof(uint32_t));
    draw.vGrid = storageBuffer("vBandGrid", encoded.vBandGrid.data(),
                               encoded.vBandGrid.size() * sizeof(uint32_t));

    // Uniforms: exactly the production populateFillUniform values for a solid fill.
    SolidFillUniforms uniforms = {};
    BuildSolidFillMvp(pixelFromScene, uniforms.mvp);
    BuildIdentity4x4(uniforms.patternFromPath);
    uniforms.viewport[0] = static_cast<float>(kBaselineSize);
    uniforms.viewport[1] = static_cast<float>(kBaselineSize);
    uniforms.tileSize[0] = 1.0f;
    uniforms.tileSize[1] = 1.0f;
    const float alpha = spec.color.a / 255.0f;
    uniforms.color[0] = (spec.color.r / 255.0f) * alpha;
    uniforms.color[1] = (spec.color.g / 255.0f) * alpha;
    uniforms.color[2] = (spec.color.b / 255.0f) * alpha;
    uniforms.color[3] = alpha;
    uniforms.fillRule = (spec.rule == FillRule::EvenOdd) ? 1u : 0u;
    uniforms.paintMode = 0;
    uniforms.patternOpacity = 1.0f;
    uniforms.gridYBase = encoded.yBase;
    uniforms.gridHStride = encoded.hStride;
    uniforms.gridHBandCount = encoded.hBandCount;
    uniforms.gridXBase = encoded.xBase;
    uniforms.gridVStride = encoded.vStride;
    uniforms.gridVBandCount = encoded.vBandCount;
    WriteBoundingPolygon(encoded, uniforms);

    draw.uniformBuffer =
        unwrap(device_->createBuffer(BufferDescriptor{"uniforms", sizeof(SolidFillUniforms),
                                                      BufferUsage::Uniform | BufferUsage::CopyDst}),
               "createBuffer uniforms");
    const Status uniformWrite = device_->writeBuffer(
        draw.uniformBuffer, 0,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&uniforms), sizeof(uniforms)));
    ASSERT_FALSE(uniformWrite.hasError()) << uniformWrite.error();

    // Storage buffers bind their FULL created byte size: the fragment shader indexes past
    // element 0, and Vulkan enforces the bound range (Metal ignores it, but both slices bind
    // the same honest sizes).
    std::vector<BindGroupEntry> entries;
    entries.push_back({0, BufferBinding{draw.uniformBuffer, 0, sizeof(SolidFillUniforms)}});
    entries.push_back({1, BufferBinding{draw.bands.buffer, 0, draw.bands.sizeBytes}});
    entries.push_back({2, BufferBinding{draw.curves.buffer, 0, draw.curves.sizeBytes}});
    entries.push_back({3, TextureViewBinding{dummyView}});
    entries.push_back({4, SamplerBinding{dummySampler}});
    entries.push_back({5, TextureViewBinding{dummyView}});
    entries.push_back({6, SamplerBinding{dummySampler}});
    entries.push_back(
        {7, BufferBinding{instanceTransforms.buffer, 0, instanceTransforms.sizeBytes}});
    entries.push_back({8, BufferBinding{draw.vBands.buffer, 0, draw.vBands.sizeBytes}});
    entries.push_back({9, BufferBinding{draw.vCurves.buffer, 0, draw.vCurves.sizeBytes}});
    entries.push_back({10, BufferBinding{draw.hGrid.buffer, 0, draw.hGrid.sizeBytes}});
    entries.push_back({11, BufferBinding{draw.vGrid.buffer, 0, draw.vGrid.sizeBytes}});
    draw.bindGroup = unwrap(device_->createBindGroup(BindGroupDescriptor{
                                "solidFillGroup", bindGroupLayout, std::move(entries)}),
                            "createBindGroup");
    draws.push_back(std::move(draw));
  }

  // ----- Encode: clear to transparent, draw the three paths, read back -----
  std::unique_ptr<CommandEncoder> encoder =
      unwrap(device_->createCommandEncoder(), "createCommandEncoder");
  Result<RenderPassEncoder*> passResult = encoder->beginRenderPass(RenderPassDescriptor{
      "baselinePass",
      {RenderPassColorAttachment{targetView, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 0}}}});
  ASSERT_FALSE(passResult.hasError()) << passResult.error();
  RenderPassEncoder* pass = passResult.result();

  for (const PathDraw& draw : draws) {
    const Status pipelineStatus = pass->setPipeline(pipeline);
    ASSERT_FALSE(pipelineStatus.hasError()) << pipelineStatus.error();
    const Status bindGroupStatus = pass->setBindGroup(0, draw.bindGroup);
    ASSERT_FALSE(bindGroupStatus.hasError()) << bindGroupStatus.error();
    const Status drawStatus = pass->draw(draw.vertexCount);
    ASSERT_FALSE(drawStatus.hasError()) << drawStatus.error();
  }
  const Status endStatus = pass->end();
  ASSERT_FALSE(endStatus.hasError()) << endStatus.error();
  const Status copyStatus = encoder->copyTextureToBuffer(
      TexelCopyTextureInfo{target}, readback, TexelCopyBufferLayout{0, kBytesPerRow, kBaselineSize},
      Extent2d{kBaselineSize, kBaselineSize});
  ASSERT_FALSE(copyStatus.hasError()) << copyStatus.error();

  Result<CommandBuffer> commands = encoder->finish();
  ASSERT_FALSE(commands.hasError()) << commands.error();
  Result<uint64_t> serial = device_->submit(std::move(commands).result());
  ASSERT_FALSE(serial.hasError()) << serial.error();

  // The command buffer must complete without a Metal execution error.
  ASSERT_TRUE(device_->waitForSerial(serial.result(), /*timeoutSeconds=*/30.0))
      << "Command buffer did not complete cleanly: " << device_->lastErrorForTest();
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());

  // ----- Pixel comparison against the frozen baseline -----
  Result<std::vector<uint8_t>> pixels = device_->readBackBuffer(readback);
  ASSERT_FALSE(pixels.hasError()) << pixels.error();

  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(static_cast<int>(kBaselineSize), static_cast<int>(kBaselineSize));
  bitmap.pixels = std::move(pixels).result();
  bitmap.rowBytes = kBytesPerRow;
  bitmap.alphaType = svg::AlphaType::Premultiplied;

  // Strict identity: the Metal slice must reproduce the frozen baseline byte-for-byte (zero
  // mismatched pixels, anti-aliased pixels included).
  editor::tests::CompareBitmapToGolden(bitmap, goldenPath, "metal_solid_fill",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::gpu::metal::tests
