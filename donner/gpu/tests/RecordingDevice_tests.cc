/// @file
/// Recording backend tests: submission serials, deterministic serialization, and the golden
/// capture of a representative command stream.

#include "donner/gpu/RecordingDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::Eq;
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

/// Backend fake whose submissions always fail, proving that a rejected submission does not
/// consume a serial.
class FailingSubmitDevice : public Device {
public:
  uint64_t completedSerial() const override { return lastSubmittedSerial(); }

protected:
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
  void onDestroyResource(std::string_view, uint32_t) override {}
  Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override { return OkStatus(); }
  Status onWriteTexture(uint32_t, std::span<const uint8_t>, const TexelCopyBufferLayout&,
                        const Extent2d&) override {
    return OkStatus();
  }
  Status onSubmit(uint64_t, uint32_t, std::span<const Command>) override {
    return GpuError{GpuErrorType::InvalidState, "backend rejected the submission"};
  }
};

/// Records the representative solid-fill stream: every resource creation, queue write, a full
/// render pass, a readback copy, a destroy, and a submission, followed by the RAII teardown of
/// every remaining handle in reverse declaration order as the locals go out of scope.
void RecordRepresentativeStream(RecordingDevice& device) {
  const Texture target = GetResultOrFail(device.createTexture(
      TextureDescriptor{"target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm,
                        TextureUsage::RenderAttachment | TextureUsage::CopySrc}));
  const TextureView targetView =
      GetResultOrFail(device.createTextureView(target, TextureViewDescriptor{"targetView"}));
  const Texture image = GetResultOrFail(
      device.createTexture(TextureDescriptor{"image", Extent2d{4, 4}, TextureFormat::RGBA8Unorm,
                                             TextureUsage::Sampled | TextureUsage::CopyDst}));
  const Sampler linearSampler [[maybe_unused]] = GetResultOrFail(
      device.createSampler(SamplerDescriptor{"linearClamp", FilterMode::Linear, FilterMode::Linear,
                                             AddressMode::ClampToEdge, AddressMode::ClampToEdge}));

  const Buffer vertexBuffer = GetResultOrFail(device.createBuffer(
      BufferDescriptor{"vertices", 48, BufferUsage::Vertex | BufferUsage::CopyDst}));
  const Buffer uniformBuffer = GetResultOrFail(device.createBuffer(
      BufferDescriptor{"uniforms", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  const Buffer readbackBuffer = GetResultOrFail(device.createBuffer(
      BufferDescriptor{"readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));
  Buffer scratchBuffer =
      GetResultOrFail(device.createBuffer(BufferDescriptor{"scratch", 8, BufferUsage::CopySrc}));

  const BindGroupLayout bindGroupLayout =
      GetResultOrFail(device.createBindGroupLayout(BindGroupLayoutDescriptor{
          "solidBindings",
          {BindGroupLayoutEntry{0, ShaderStage::Vertex | ShaderStage::Fragment,
                                BindingType::UniformBuffer}}}));
  const PipelineLayout pipelineLayout = GetResultOrFail(
      device.createPipelineLayout(PipelineLayoutDescriptor{"solidLayout", {bindGroupLayout}}));
  const BindGroup bindGroup = GetResultOrFail(device.createBindGroup(BindGroupDescriptor{
      "solidUniforms", bindGroupLayout, {BindGroupEntry{0, BufferBinding{uniformBuffer, 0, 16}}}}));
  const ShaderModule shader = GetResultOrFail(device.createShaderModule(ShaderModuleDescriptor{
      "solidFill", "@vertex fn vsMain() {}\n@fragment fn fsMain() {}", ShaderSourceKind::Wgsl}));

  const RenderPipeline pipeline =
      GetResultOrFail(device.createRenderPipeline(RenderPipelineDescriptor{
          "solid", pipelineLayout,
          VertexState{
              shader,
              "vsMain",
              {VertexBufferLayout{
                  8, VertexStepMode::Vertex, {VertexAttribute{VertexFormat::Float32x2, 0, 0}}}}},
          FragmentState{
              shader,
              "fsMain",
              {ColorTargetState{
                  TextureFormat::RGBA8Unorm,
                  BlendState{BlendComponent{BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                                            BlendOperation::Add},
                             BlendComponent{BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                                            BlendOperation::Add}}}}},
          PrimitiveTopology::TriangleList, CullMode::None}));

  EXPECT_THAT(device.writeBuffer(vertexBuffer, 0, MakeBytes(48)), IsOk());
  EXPECT_THAT(device.writeBuffer(uniformBuffer, 0, MakeBytes(16)), IsOk());
  EXPECT_THAT(device.writeTexture(image, MakeBytes(3 * 256 + 16), TexelCopyBufferLayout{0, 256, 4},
                                  Extent2d{4, 4}),
              IsOk());
  EXPECT_THAT(device.destroyBuffer(std::move(scratchBuffer)), IsOk());

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
  RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(RenderPassDescriptor{
      "mainPass",
      {RenderPassColorAttachment{targetView, LoadOp::Clear, StoreOp::Store, {0, 0, 0.5, 1}}}}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setPipeline(pipeline), IsOk());
  EXPECT_THAT(pass->setBindGroup(0, bindGroup), IsOk());
  EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer), IsOk());
  EXPECT_THAT(pass->setScissorRect(0, 0, 4, 4), IsOk());
  EXPECT_THAT(pass->setViewport(0, 0, 4, 4, 0, 1), IsOk());
  EXPECT_THAT(pass->draw(6), IsOk());
  EXPECT_THAT(pass->end(), IsOk());
  EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{target}, readbackBuffer,
                                           TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsOk());

  auto commandBuffer = encoder->finish();
  ASSERT_THAT(commandBuffer, HasResult());
  EXPECT_THAT(device.submit(std::move(commandBuffer).result()), HasResult());
}

TEST(RecordingDeviceTests, SubmissionSerialsStrictlyIncreaseAndComplete) {
  RecordingDevice device;
  EXPECT_THAT(device.lastSubmittedSerial(), Eq(0u));
  EXPECT_THAT(device.completedSerial(), Eq(0u));

  uint64_t previousSerial = 0;
  for (int i = 0; i < 3; ++i) {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
    auto commandBuffer = encoder->finish();
    ASSERT_THAT(commandBuffer, HasResult());

    auto serial = device.submit(std::move(commandBuffer).result());
    ASSERT_THAT(serial, HasResult());
    EXPECT_THAT(serial.result(), Eq(previousSerial + 1));
    previousSerial = serial.result();

    EXPECT_THAT(device.lastSubmittedSerial(), Eq(previousSerial));
    EXPECT_THAT(device.completedSerial(), Eq(previousSerial));
  }
}

TEST(RecordingDeviceTests, FailedBackendSubmitDoesNotBurnSerial) {
  FailingSubmitDevice device;
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
  auto commandBuffer = encoder->finish();
  ASSERT_THAT(commandBuffer, HasResult());

  EXPECT_THAT(device.submit(std::move(commandBuffer).result()),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                    HasSubstr("backend rejected the submission")));

  // The rejected submission must not consume a serial: completion waiters would otherwise treat
  // the failed work as finished.
  EXPECT_THAT(device.lastSubmittedSerial(), Eq(0u));
  EXPECT_THAT(device.completedSerial(), Eq(0u));
}

TEST(RecordingDeviceTests, IdenticalStreamsSerializeByteIdentically) {
  RecordingDevice first;
  RecordingDevice second;
  RecordRepresentativeStream(first);
  RecordRepresentativeStream(second);

  EXPECT_THAT(first.serialize(), Eq(second.serialize()));
}

TEST(RecordingDeviceTests, RepeatedStreamOnOneDeviceAppendsIdenticalCapture) {
  RecordingDevice device;
  RecordRepresentativeStream(device);
  const std::string once = device.serialize();

  // Serials and slots keep advancing, so run a fresh device to compare the repeat.
  RecordingDevice repeatDevice;
  RecordRepresentativeStream(repeatDevice);
  EXPECT_THAT(repeatDevice.serialize(), Eq(once));
}

TEST(RecordingDeviceTests, GoldenSerializationOfRepresentativeStream) {
  RecordingDevice device;
  RecordRepresentativeStream(device);

  // clang-format off
  const std::string kExpected =
      R"(createTexture texture#0 label="target" size=4x4 format=RGBA8Unorm usage=RenderAttachment|CopySrc sampleCount=1
createTextureView textureView#0 label="targetView" texture=texture#0
createTexture texture#1 label="image" size=4x4 format=RGBA8Unorm usage=Sampled|CopyDst sampleCount=1
createSampler sampler#0 label="linearClamp" magFilter=Linear minFilter=Linear addressModeU=ClampToEdge addressModeV=ClampToEdge
createBuffer buffer#0 label="vertices" byteSize=48 usage=Vertex|CopyDst
createBuffer buffer#1 label="uniforms" byteSize=16 usage=Uniform|CopyDst
createBuffer buffer#2 label="readback" byteSize=1024 usage=CopyDst|MapRead
createBuffer buffer#3 label="scratch" byteSize=8 usage=CopySrc
createBindGroupLayout bindGroupLayout#0 label="solidBindings" entries=[{binding=0 visibility=Vertex|Fragment type=UniformBuffer}]
createPipelineLayout pipelineLayout#0 label="solidLayout" bindGroupLayouts=[bindGroupLayout#0]
createBindGroup bindGroup#0 label="solidUniforms" layout=bindGroupLayout#0 entries=[{binding=0 buffer=buffer#1 offsetBytes=0 sizeBytes=16}]
createShaderModule shaderModule#0 label="solidFill" sourceKind=Wgsl sourceBytes=47 sourceHash=b5b881c1bdd08531
createRenderPipeline renderPipeline#0 label="solid" layout=pipelineLayout#0 vertex={module=shaderModule#0 entryPoint="vsMain" buffers=[{strideBytes=8 stepMode=Vertex attributes=[{format=Float32x2 offsetBytes=0 shaderLocation=0}]}]} fragment={module=shaderModule#0 entryPoint="fsMain" targets=[{format=RGBA8Unorm blend={color={srcFactor=One dstFactor=OneMinusSrcAlpha operation=Add} alpha={srcFactor=One dstFactor=OneMinusSrcAlpha operation=Add}} writeMask=Red|Green|Blue|Alpha}]} topology=TriangleList cullMode=None multisampleCount=1
writeBuffer buffer#0 offsetBytes=0 byteCount=48 dataHash=dd7a5e9540df1b95
writeBuffer buffer#1 offsetBytes=0 byteCount=16 dataHash=7c84dc9477851775
writeTexture texture#1 offsetBytes=0 bytesPerRow=256 rowsPerImage=4 writeSize=4x4 byteCount=784 dataHash=aaaef608c2729075
destroy buffer#3
submit serial=1 commandBuffer#0 commandCount=9
  beginRenderPass label="mainPass" colorAttachments=[{view=textureView#0 loadOp=Clear storeOp=Store clearColor=(0 0 0.5 1)}]
  setPipeline renderPipeline#0
  setBindGroup index=0 bindGroup=bindGroup#0
  setVertexBuffer slot=0 buffer=buffer#0 offsetBytes=0
  setScissorRect x=0 y=0 width=4 height=4
  setViewport x=0 y=0 width=4 height=4 minDepth=0 maxDepth=1
  draw vertexCount=6 instanceCount=1 firstVertex=0 firstInstance=0
  endRenderPass
  copyTextureToBuffer texture=texture#0 buffer=buffer#2 offsetBytes=0 bytesPerRow=256 rowsPerImage=4 copySize=4x4
destroy renderPipeline#0
destroy shaderModule#0
destroy bindGroup#0
destroy pipelineLayout#0
destroy bindGroupLayout#0
destroy buffer#2
destroy buffer#1
destroy buffer#0
destroy sampler#0
destroy texture#1
destroy textureView#0
destroy texture#0
)";
  // clang-format on

  EXPECT_THAT(device.serialize(), Eq(kExpected));
}

TEST(RecordingDeviceTests, SerializationContainsNoPointers) {
  RecordingDevice device;
  RecordRepresentativeStream(device);

  EXPECT_THAT(device.serialize(), Not(HasSubstr("0x")));
}

}  // namespace
}  // namespace donner::gpu
