/// @file
/// Indexed draws through the Vulkan backend: the shared indexed scene renders exactly on every
/// device the backend accepts (both index formats, or Uint16 plus a refused Uint32 without the
/// full 32-bit range), an empty index binding at the buffer end submits without a native bind,
/// and an index range the encoder rejects never reaches the device.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/VertexInputSlice.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan::tests {
namespace {

using testing::Ge;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;
using testing::NotNull;
using testing::SizeIs;

/// Byte size of the one-quad index buffer; the exact-end binding cases bind at this offset.
constexpr uint64_t kQuadIndexBytes = 8 * sizeof(uint16_t);
/// Row pitch of the 4x4 readback; 256 keeps the copy layout shared with the other native scenes.
constexpr uint32_t kReadbackRowBytes = 256;

class VulkanIndexedDrawTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::Create();
    if (!device_) {
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available; the "
                  "indexed-draw gate must not be skipped on this runner";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  /// Emits BuildVertexInputModule as SPIR-V; callers unwrap with `ASSERT_THAT(_, HasResult())`.
  static shader::ShaderResult<ShaderModuleDescriptor> VertexInputShader() {
    auto module = gpu::tests::BuildVertexInputModule();
    if (module.hasError()) {
      return std::move(module).error();
    }
    auto emitted = shader::EmitSpirv(module.result());
    if (emitted.hasError()) {
      return std::move(emitted).error();
    }
    return ShaderModuleDescriptor{
        "attributes", {}, ShaderSourceKind::Spirv, std::move(emitted).result()};
  }

  /// Runs the shared indexed scene the way the device's index-range capability dictates: both
  /// formats when Uint32 carries its full range, otherwise Uint16 pixels plus a refused Uint32.
  void checkAcceptanceScene() {
    const auto shader = VertexInputShader();
    ASSERT_THAT(shader, HasResult());
    const auto readback = [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); };
    if (device_->supportsFullIndexRange(IndexFormat::Uint32)) {
      SCOPED_TRACE("device honors the full Uint32 range: both index formats bound");
      gpu::tests::CheckIndexedDrawScene(*device_, shader.result(), readback,
                                        gpu::tests::IndexedSceneFormats::Uint16AndUint32);
    } else {
      SCOPED_TRACE("device lacks the full Uint32 range: Uint16 pixels, Uint32 refused");
      gpu::tests::CheckIndexedDrawScene(*device_, shader.result(), readback,
                                        gpu::tests::IndexedSceneFormats::Uint16Only);
      ASSERT_NO_FATAL_FAILURE(createQuadResources());
      std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
      RenderPassEncoder* pass = beginQuadPass(*encoder);
      ASSERT_THAT(pass, NotNull());
      EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint32),
                  IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("Uint32")));
      // The refusal poisons the encoder, so nothing reaches the device.
      EXPECT_THAT(encoder->finish(), IsGpuError(GpuErrorType::Unsupported));
    }
    EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
  }

  /// Creates the pipeline, a full-clip quad at slot 0, the shared instance upload at slot 1, one
  /// quad of 16-bit indices, and a 4x4 target with its readback buffer, all held by the fixture.
  void createQuadResources() {
    const auto shader = VertexInputShader();
    ASSERT_THAT(shader, HasResult());
    shaderModule_ = GetResultOrFail(device_->createShaderModule(shader.result()));
    layout_ =
        GetResultOrFail(device_->createPipelineLayout(PipelineLayoutDescriptor{"attributes", {}}));
    pipeline_ = GetResultOrFail(device_->createRenderPipeline(
        gpu::tests::VertexInputPipelineDescriptor(shaderModule_, layout_)));
    const std::array<gpu::tests::VertexInputPosition, 4> vertices{
        {{{77, 77}, {-1, -1}}, {{77, 77}, {1, -1}}, {{77, 77}, {1, 1}}, {{77, 77}, {-1, 1}}}};
    const gpu::tests::VertexInputInstanceUpload instances = gpu::tests::VertexInputInstances();
    const std::array<uint16_t, 8> indices{0, 1, 2, 0, 2, 3, 0, 0};
    static_assert(sizeof(indices) == kQuadIndexBytes);
    vertexBuffer_ = GetResultOrFail(device_->createBuffer(BufferDescriptor{
        "positions", sizeof(vertices), BufferUsage::Vertex | BufferUsage::CopyDst}));
    instanceBuffer_ = GetResultOrFail(device_->createBuffer(BufferDescriptor{
        "instances", sizeof(instances), BufferUsage::Vertex | BufferUsage::CopyDst}));
    indexBuffer_ = GetResultOrFail(device_->createBuffer(
        BufferDescriptor{"indices", kQuadIndexBytes, BufferUsage::Index | BufferUsage::CopyDst}));
    ASSERT_THAT(device_->writeBuffer(vertexBuffer_, 0, gpu::tests::VertexInputBytes(vertices)),
                IsOk());
    ASSERT_THAT(device_->writeBuffer(instanceBuffer_, 0, gpu::tests::VertexInputBytes(instances)),
                IsOk());
    ASSERT_THAT(device_->writeBuffer(indexBuffer_, 0, gpu::tests::VertexInputBytes(indices)),
                IsOk());
    target_ = GetResultOrFail(device_->createTexture(
        TextureDescriptor{"target",
                          {4, 4},
                          TextureFormat::RGBA8Unorm,
                          TextureUsage::RenderAttachment | TextureUsage::CopySrc}));
    view_ = GetResultOrFail(device_->createTextureView(target_, TextureViewDescriptor{"target"}));
    readback_ = GetResultOrFail(device_->createBuffer(BufferDescriptor{
        "pixels", kReadbackRowBytes * 4, BufferUsage::CopyDst | BufferUsage::MapRead}));
  }

  /// Begins a pass that clears the target to blue, with the pipeline and both vertex slots bound.
  RenderPassEncoder* beginQuadPass(CommandEncoder& encoder) {
    RenderPassEncoder* pass = GetResultOrFail(encoder.beginRenderPass(
        RenderPassDescriptor{"indexed", {{view_, LoadOp::Clear, StoreOp::Store, {0, 0, 1, 1}}}}));
    if (pass == nullptr) {
      return nullptr;
    }
    EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(1, instanceBuffer_,
                                      offsetof(gpu::tests::VertexInputInstanceUpload, records)),
                IsOk());
    return pass;
  }

  /// Finishes \p encoder, submits it, and waits for the submission to complete.
  void submitAndWait(CommandEncoder& encoder) {
    auto commands = encoder.finish();
    ASSERT_THAT(commands, HasResult());
    auto serial = device_->submit(std::move(commands).result());
    ASSERT_THAT(serial, HasResult()) << "device error: " << device_->lastErrorForTest();
    ASSERT_THAT(device_->waitForSerial(serial.result(), 5.0), IsTrue())
        << "device error: " << device_->lastErrorForTest();
  }

  /// Reads the 4x4 target back through \ref readback_ and compares it against a solid \p color.
  void expectTargetIsSolid(std::array<uint8_t, 4> color, std::string_view name) {
    const auto bytes = device_->readBackBuffer(readback_);
    ASSERT_THAT(bytes, HasResult());
    ASSERT_THAT(bytes.result(), SizeIs(Ge(kReadbackRowBytes * 4)));
    svg::RendererBitmap actual;
    actual.dimensions = {4, 4};
    actual.rowBytes = kReadbackRowBytes;
    actual.pixels = bytes.result();
    svg::RendererBitmap expected;
    expected.dimensions = actual.dimensions;
    expected.rowBytes = actual.rowBytes;
    expected.pixels.resize(expected.rowBytes * 4);
    for (uint32_t y = 0; y < 4; ++y) {
      for (uint32_t x = 0; x < 4; ++x) {
        std::copy(color.begin(), color.end(),
                  expected.pixels.begin() + y * expected.rowBytes + x * 4);
      }
    }
    editor::tests::CompareBitmapToBitmap(actual, expected, name,
                                         editor::tests::PixelmatchIdentityParams());
  }

  std::unique_ptr<VulkanDevice> device_;
  ShaderModule shaderModule_;
  PipelineLayout layout_;
  RenderPipeline pipeline_;
  Buffer vertexBuffer_;
  Buffer instanceBuffer_;
  Buffer indexBuffer_;
  Texture target_;
  TextureView view_;
  Buffer readback_;
};

TEST_F(VulkanIndexedDrawTest, IndexedQuadsWithOffsetsInstancingAndScissorMatchTheExpectedImage) {
  checkAcceptanceScene();
}

TEST_F(VulkanIndexedDrawTest, WithoutTheFullUint32RangeTheSceneRendersFromUint16AndRefusesUint32) {
  device_->disableFullUint32IndexRangeForTest();
  ASSERT_THAT(device_->supportsFullIndexRange(IndexFormat::Uint32), IsFalse());
  checkAcceptanceScene();
}

TEST_F(VulkanIndexedDrawTest, SixteenBitIndicesAlwaysCarryTheirFullRange) {
  EXPECT_THAT(device_->supportsFullIndexRange(IndexFormat::Uint16), IsTrue());
}

TEST_F(VulkanIndexedDrawTest, AnIndexRangePastTheBoundBufferNeverReachesTheDevice) {
  const auto shader = VertexInputShader();
  ASSERT_THAT(shader, HasResult());
  const ShaderModule shaderModule = GetResultOrFail(device_->createShaderModule(shader.result()));
  const PipelineLayout layout =
      GetResultOrFail(device_->createPipelineLayout(PipelineLayoutDescriptor{"attributes", {}}));
  const RenderPipeline pipeline = GetResultOrFail(device_->createRenderPipeline(
      gpu::tests::VertexInputPipelineDescriptor(shaderModule, layout)));
  const Buffer vertexBuffer = GetResultOrFail(device_->createBuffer(BufferDescriptor{
      "positions", 4 * sizeof(gpu::tests::VertexInputPosition), BufferUsage::Vertex}));
  const Buffer instanceBuffer = GetResultOrFail(device_->createBuffer(BufferDescriptor{
      "instances", 2 * sizeof(gpu::tests::VertexInputInstance), BufferUsage::Vertex}));
  // Eight 16-bit indices; the draw below asks for nine.
  const Buffer indexBuffer =
      GetResultOrFail(device_->createBuffer(BufferDescriptor{"indices", 16, BufferUsage::Index}));
  const Texture target = GetResultOrFail(device_->createTexture(TextureDescriptor{
      "target", {4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view =
      GetResultOrFail(device_->createTextureView(target, TextureViewDescriptor{"target"}));

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
  RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(
      RenderPassDescriptor{"indexed", {{view, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}}}}));
  ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(0, vertexBuffer), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(1, instanceBuffer), IsOk());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(9),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("index range [0, 9)")));
  EXPECT_THAT(pass->end(), IsGpuError(GpuErrorType::OutOfBounds));
  EXPECT_THAT(encoder->finish(), IsGpuError(GpuErrorType::OutOfBounds));
  // Nothing was submitted, so the validation layers observed no command buffer at all.
  EXPECT_THAT(device_->lastSubmittedSerial(), testing::Eq(uint64_t{0}));
  EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
}

// The encoder accepts a binding exactly at the buffer end as an empty range, while Vulkan
// requires vkCmdBindIndexBuffer's offset to be below the buffer size. The validation layer latches
// an invalid native bind into lastErrorForTest, so these cases fail whenever the backend binds an
// empty range natively instead of waiting for a draw that needs the range.
TEST_F(VulkanIndexedDrawTest, AnEmptyIndexBindingAtTheBufferEndWithNoDrawSubmitsCleanly) {
  ASSERT_NO_FATAL_FAILURE(createQuadResources());
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
  RenderPassEncoder* pass = beginQuadPass(*encoder);
  ASSERT_THAT(pass, NotNull());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16, kQuadIndexBytes), IsOk());
  ASSERT_THAT(pass->end(), IsOk());
  ASSERT_NO_FATAL_FAILURE(submitAndWait(*encoder));
  EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
}

TEST_F(VulkanIndexedDrawTest, AnEmptyIndexBindingAtTheBufferEndWithAZeroCountDrawSubmitsCleanly) {
  ASSERT_NO_FATAL_FAILURE(createQuadResources());
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
  RenderPassEncoder* pass = beginQuadPass(*encoder);
  ASSERT_THAT(pass, NotNull());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16, kQuadIndexBytes), IsOk());
  ASSERT_THAT(pass->drawIndexed(0, 1, 0, 0, 1), IsOk());
  ASSERT_THAT(pass->end(), IsOk());
  ASSERT_NO_FATAL_FAILURE(submitAndWait(*encoder));
  EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
}

TEST_F(VulkanIndexedDrawTest, ANonemptyRebindAfterAnEmptyBindingDrawsFromTheRebind) {
  ASSERT_NO_FATAL_FAILURE(createQuadResources());
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
  RenderPassEncoder* pass = beginQuadPass(*encoder);
  ASSERT_THAT(pass, NotNull());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16, kQuadIndexBytes), IsOk());
  ASSERT_THAT(pass->drawIndexed(0, 1, 0, 0, 1), IsOk());
  // The rebind at offset 0 covers the whole quad; instance record 1 paints it red.
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  ASSERT_THAT(pass->drawIndexed(6, 1, 0, 0, 1), IsOk());
  // An empty binding after the real draw must not disturb it either.
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16, kQuadIndexBytes), IsOk());
  ASSERT_THAT(pass->drawIndexed(0, 1, 0, 0, 1), IsOk());
  ASSERT_THAT(pass->end(), IsOk());
  ASSERT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{target_}, readback_,
                                           {0, kReadbackRowBytes, 4}, {4, 4}),
              IsOk());
  ASSERT_NO_FATAL_FAILURE(submitAndWait(*encoder));
  EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
  expectTargetIsSolid({255, 0, 0, 255}, "indexed_rebind_after_empty_binding");
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
