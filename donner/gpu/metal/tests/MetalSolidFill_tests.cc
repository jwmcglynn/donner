/// @file
/// The Metal solid-fill vertical slice (design 0053 phase 3): renders the shared baseline scene
/// through donner::gpu::metal::MetalDevice with the MSL emitted from the solid-fill IR program,
/// and compares pixels against the frozen baseline captured from the current production
/// renderer.
///
/// The geometry, uniforms, and draw sequence mirror the production encoder's fillPath data flow
/// exactly: GeodePathEncoder banding, the same clip-space MVP construction (pixel -> clip with
/// the Y flip for a top-left origin), premultiplied colors, an identity per-instance transform
/// at binding 7, and 1x1 dummy pattern/clip textures at bindings 3..6.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/shader/MslEmitter.h"
#include "donner/gpu/shader/programs/SolidFill.h"
#include "donner/gpu/tests/BaselineScene.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"

using testing::HasSubstr;

namespace donner::gpu::metal::tests {
namespace {

using geode::EncodedPath;
using gpu::tests::BaselinePathSpec;
using gpu::tests::BaselinePixelFromScene;
using gpu::tests::BaselineScenePaths;
using gpu::tests::kBaselineSize;

constexpr uint32_t kBytesPerRow = kBaselineSize * 4;  // 1024; already 256-byte aligned.

/// C++ mirror of the shader's 288-byte Uniforms struct (layout anchored by the shader IR layout
/// tests; field order matches slug_fill/the solid-fill IR program).
struct alignas(16) Uniforms {
  float mvp[16];                //!< Column-major clip-from-scene matrix.
  float patternFromPath[16];    //!< Pattern transform (identity for solid fills).
  float viewport[2];            //!< Viewport size in pixels.
  float tileSize[2];            //!< Pattern tile size (unused for solid fills).
  float color[4];               //!< Premultiplied fill color.
  uint32_t fillRule;            //!< 0 = non-zero, 1 = even-odd.
  uint32_t paintMode;           //!< 0 = solid color.
  float patternOpacity;         //!< 1.0 for solid fills.
  uint32_t hasClipPolygon;      //!< 0 = no clip polygon.
  uint32_t hasClipMask;         //!< 0 = no clip mask.
  uint32_t pad0;                //!< Padding to the grid block.
  uint32_t pad1;                //!< Padding.
  uint32_t pad2;                //!< Padding.
  float gridYBase;              //!< Horizontal band grid base.
  float gridHStride;            //!< Horizontal band stride.
  uint32_t gridHBandCount;      //!< Horizontal band count.
  float gridXBase;              //!< Vertical band grid base.
  float gridVStride;            //!< Vertical band stride.
  uint32_t gridVBandCount;      //!< Vertical band count.
  uint32_t gridPad0;            //!< Padding.
  uint32_t gridPad1;            //!< Padding.
  float clipPolygonPlanes[16];  //!< Four vec4 half-planes (unused: hasClipPolygon == 0).
};
static_assert(sizeof(Uniforms) == 288, "Uniforms must match the shader layout");

/// Builds the same clip-space MVP the production encoder computes: scene -> pixel via
/// \p pixelFromScene, then pixel -> clip with x_clip = 2x/W - 1 and y_clip = -2y/H + 1 (the Y
/// flip for a top-left pixel origin). Column-major mat4.
void BuildMvp(const Transform2d& pixelFromScene, float* out16) {
  const double sx = 2.0 / static_cast<double>(kBaselineSize);
  const double sy = -2.0 / static_cast<double>(kBaselineSize);
  const double a = pixelFromScene.data[0];
  const double b = pixelFromScene.data[1];
  const double c = pixelFromScene.data[2];
  const double d = pixelFromScene.data[3];
  const double e = pixelFromScene.data[4];
  const double f = pixelFromScene.data[5];

  std::memset(out16, 0, 16 * sizeof(float));
  out16[0] = static_cast<float>(sx * a);
  out16[1] = static_cast<float>(sy * b);
  out16[4] = static_cast<float>(sx * c);
  out16[5] = static_cast<float>(sy * d);
  out16[10] = 1.0f;
  out16[12] = static_cast<float>(sx * e - 1.0);
  out16[13] = static_cast<float>(sy * f + 1.0);
  out16[15] = 1.0f;
}

/// Writes an identity 4x4 into \p out16 (column-major).
void BuildIdentity(float* out16) {
  std::memset(out16, 0, 16 * sizeof(float));
  out16[0] = out16[5] = out16[10] = out16[15] = 1.0f;
}

/// A storage buffer plus the byte size it was created with, so bind groups can bind the FULL
/// range honestly. Vulkan enforces VkDescriptorBufferInfo.range, so binding a smaller range
/// than the shader indexes would read out of bounds; Metal ignores the range, but both slices
/// bind the same honest sizes.
struct SizedBuffer {
  Buffer buffer;          //!< The storage buffer.
  uint64_t byteSize = 0;  //!< Byte size the buffer was created with.
};

/// One path's GPU resources.
struct PathDraw {
  Buffer vertexBuffer;       //!< Quad vertices (6 x 20 bytes).
  Buffer uniformBuffer;      //!< 288-byte Uniforms.
  SizedBuffer bands;         //!< Horizontal bands (or 4-byte dummy).
  SizedBuffer curves;        //!< Horizontal curves (or dummy).
  SizedBuffer vBands;        //!< Vertical bands (or dummy).
  SizedBuffer vCurves;       //!< Vertical curves (or dummy).
  SizedBuffer hGrid;         //!< Horizontal band grid (or dummy).
  SizedBuffer vGrid;         //!< Vertical band grid (or dummy).
  BindGroup bindGroup;       //!< The 12-entry solid-fill bind group.
  uint32_t vertexCount = 0;  //!< Draw vertex count.
};

class MetalSolidFillTest : public testing::Test {
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

  /// Creates a storage buffer holding \p bytes (or a 4-byte zero dummy when empty), mirroring
  /// the production encoder's empty-region dummies (the shader's band-count gates never
  /// dereference them). Returns the buffer with its created byte size so bind groups bind the
  /// full range.
  SizedBuffer storageBuffer(const char* label, const void* data, size_t byteCount) {
    const uint32_t dummy = 0;
    if (byteCount == 0) {
      data = &dummy;
      byteCount = sizeof(dummy);
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

TEST_F(MetalSolidFillTest, MatchesFrozenBaseline) {
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

  // Vertex layout: pos (vec2f) + normal (vec2f) + bandIndex (u32) = 20 bytes.
  RenderPipelineDescriptor pipelineDescriptor{
      "solidFill", pipelineLayout,
      VertexState{shaderModule,
                  "vs_main",
                  {VertexBufferLayout{20,
                                      VertexStepMode::Vertex,
                                      {VertexAttribute{VertexFormat::Float32x2, 0, 0},
                                       VertexAttribute{VertexFormat::Float32x2, 8, 1},
                                       VertexAttribute{VertexFormat::Uint32, 16, 2}}}}},
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
  const std::array<uint8_t, 4> dummyTexel = {0, 0, 0, 0};
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
    ASSERT_FALSE(encoded.quadVertices.empty());

    PathDraw draw;
    draw.vertexCount = static_cast<uint32_t>(encoded.quadVertices.size());
    draw.vertexBuffer =
        unwrap(device_->createBuffer(BufferDescriptor{
                   "vertices", encoded.quadVertices.size() * sizeof(EncodedPath::Vertex),
                   BufferUsage::Vertex | BufferUsage::CopyDst}),
               "createBuffer vertices");
    const Status vertexWrite = device_->writeBuffer(
        draw.vertexBuffer, 0,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(encoded.quadVertices.data()),
                                 encoded.quadVertices.size() * sizeof(EncodedPath::Vertex)));
    ASSERT_FALSE(vertexWrite.hasError()) << vertexWrite.error();

    draw.bands = storageBuffer("bands", encoded.bands.data(),
                               encoded.bands.size() * sizeof(EncodedPath::Band));
    draw.curves = storageBuffer("curves", encoded.curves.data(),
                                encoded.curves.size() * sizeof(EncodedPath::Curve));
    draw.vBands = storageBuffer("vBands", encoded.vBands.data(),
                                encoded.vBands.size() * sizeof(EncodedPath::Band));
    draw.vCurves = storageBuffer("vCurves", encoded.vCurves.data(),
                                 encoded.vCurves.size() * sizeof(EncodedPath::Curve));
    draw.hGrid = storageBuffer("hBandGrid", encoded.hBandGrid.data(),
                               encoded.hBandGrid.size() * sizeof(uint32_t));
    draw.vGrid = storageBuffer("vBandGrid", encoded.vBandGrid.data(),
                               encoded.vBandGrid.size() * sizeof(uint32_t));

    // Uniforms: exactly the production populateFillUniform values for a solid fill.
    Uniforms uniforms = {};
    BuildMvp(pixelFromScene, uniforms.mvp);
    BuildIdentity(uniforms.patternFromPath);
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

    draw.uniformBuffer =
        unwrap(device_->createBuffer(BufferDescriptor{"uniforms", sizeof(Uniforms),
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
    entries.push_back({0, BufferBinding{draw.uniformBuffer, 0, sizeof(Uniforms)}});
    entries.push_back({1, BufferBinding{draw.bands.buffer, 0, draw.bands.byteSize}});
    entries.push_back({2, BufferBinding{draw.curves.buffer, 0, draw.curves.byteSize}});
    entries.push_back({3, TextureViewBinding{dummyView}});
    entries.push_back({4, SamplerBinding{dummySampler}});
    entries.push_back({5, TextureViewBinding{dummyView}});
    entries.push_back({6, SamplerBinding{dummySampler}});
    entries.push_back(
        {7, BufferBinding{instanceTransforms.buffer, 0, instanceTransforms.byteSize}});
    entries.push_back({8, BufferBinding{draw.vBands.buffer, 0, draw.vBands.byteSize}});
    entries.push_back({9, BufferBinding{draw.vCurves.buffer, 0, draw.vCurves.byteSize}});
    entries.push_back({10, BufferBinding{draw.hGrid.buffer, 0, draw.hGrid.byteSize}});
    entries.push_back({11, BufferBinding{draw.vGrid.buffer, 0, draw.vGrid.byteSize}});
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
    const Status vertexBufferStatus = pass->setVertexBuffer(0, draw.vertexBuffer);
    ASSERT_FALSE(vertexBufferStatus.hasError()) << vertexBufferStatus.error();
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
  editor::tests::CompareBitmapToGolden(
      bitmap, "donner/gpu/metal/tests/testdata/solid_fill_baseline.png", "metal_solid_fill",
      editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::gpu::metal::tests
