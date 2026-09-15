#include "donner/gpu/browser/BrowserDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/browser/tests/FakeBrowserBridge.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::browser {

namespace {

using testing::AllOf;
using testing::ElementsAre;
using testing::HasSubstr;

/// A device and the bridge underneath it, so a test can both drive the runtime and see what
/// reached the browser side.
struct BrowserFixture {
  FakeBrowserBridge* bridge = nullptr;    //!< Owned by \ref device.
  std::unique_ptr<BrowserDevice> device;  //!< The device under test.
};

/// Builds a device over a ready fake bridge, failing the test if the request does not settle.
BrowserFixture MakeDevice() {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  FakeBrowserBridge* raw = bridge.get();

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.state(), BrowserDeviceRequestState::Ready);

  Result<std::unique_ptr<BrowserDevice>> device = std::move(request).take();
  EXPECT_THAT(device, HasResult());
  if (device.hasError()) {
    return BrowserFixture{};
  }
  return BrowserFixture{raw, std::move(device).result()};
}

/// A buffer descriptor that passes runtime validation.
/// @param usage Usage flags the test needs.
BufferDescriptor SimpleBuffer(BufferUsage usage) {
  return BufferDescriptor{RcString("buffer"), 256, usage};
}

/// A texture descriptor that passes runtime validation.
/// @param usage Usage flags the test needs.
TextureDescriptor SimpleTexture(TextureUsage usage) {
  return TextureDescriptor{RcString("texture"), Extent2d{4, 4}, TextureFormat::RGBA8Unorm, usage,
                           1};
}

/// A shader module descriptor carrying trusted WGSL text.
/// @param label Debug label distinguishing modules in a transcript.
ShaderModuleDescriptor SimpleShaderModule(std::string_view label) {
  ShaderModuleDescriptor descriptor;
  descriptor.label = RcString(label);
  descriptor.sourceText = RcString("@vertex fn main() {}");
  descriptor.sourceKind = ShaderSourceKind::Wgsl;
  return descriptor;
}

}  // namespace

TEST(BrowserDeviceRequest, PendingRequestYieldsNoDevice) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  bridge->requestState = BrowserDeviceRequestState::Pending;

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.state(), BrowserDeviceRequestState::Pending);
  EXPECT_THAT(std::move(request).take(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("has not settled")));
}

TEST(BrowserDeviceRequest, BrowserWithoutGpuServiceIsUnsupported) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  bridge->requestState = BrowserDeviceRequestState::Unavailable;

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.state(), BrowserDeviceRequestState::Unavailable);
  EXPECT_THAT(std::move(request).take(),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("no GPU service")));
}

TEST(BrowserDeviceRequest, RefusedRequestReportsTheBrowsersReason) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  bridge->requestState = BrowserDeviceRequestState::Failed;
  bridge->requestError = RcString("adapter is unavailable");

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.error().str(), "adapter is unavailable");
  EXPECT_THAT(
      std::move(request).take(),
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("adapter is unavailable")));
}

TEST(BrowserDeviceRequest, RequestThatNeverBeganIsAlreadyFailed) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  bridge->beginStatus = BridgeStatus::NotOwner;

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.state(), BrowserDeviceRequestState::Failed);
  EXPECT_THAT(request.error().str(), HasSubstr("another worker"));
  EXPECT_THAT(std::move(request).take(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("never begun")));
}

TEST(BrowserDeviceRequest, NoBridgeIsAlreadyFailed) {
  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(nullptr);
  EXPECT_THAT(request.state(), BrowserDeviceRequestState::Failed);
  EXPECT_THAT(std::move(request).take(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("never begun")));
}

TEST(BrowserDevice, CreatesResourcesThroughTheBridgeWithEncodedValues) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex | BufferUsage::CopyDst));
  ASSERT_THAT(buffer, HasResult());
  Result<Texture> texture =
      fixture.device->createTexture(SimpleTexture(TextureUsage::Sampled | TextureUsage::CopyDst));
  ASSERT_THAT(texture, HasResult());

  EXPECT_THAT(fixture.bridge->calls,
              ElementsAre("beginDeviceRequest", "createBuffer id=1 byteSize=256 usage=33",
                          "createTexture id=2 size=4x4 format=1 usage=10"));
}

TEST(BrowserDevice, ReleasesTheBrowserObjectWhenAResourceIsDestroyed) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer = fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex));
  ASSERT_THAT(buffer, HasResult());
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Buffer, 1), true);

  EXPECT_THAT(fixture.device->destroyBuffer(std::move(buffer).result()), IsOk());
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Buffer, 1), false);
  EXPECT_THAT(fixture.bridge->objectCount(), 0u);
}

TEST(BrowserDevice, AReusedSlotGetsAFreshIdentifierRatherThanTheRetiredOne) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> first = fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex));
  ASSERT_THAT(first, HasResult());
  ASSERT_THAT(fixture.device->destroyBuffer(std::move(first).result()), IsOk());

  // The runtime is free to hand the same slot to the next buffer; the browser must not be handed
  // the same identifier, or a stale reference would address the new object.
  Result<Buffer> second = fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex));
  ASSERT_THAT(second, HasResult());

  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Buffer, 1), false);
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Buffer, 2), true);
}

TEST(BrowserDevice, ReleasesEveryBrowserObjectWhenTheDeviceIsDestroyed) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  FakeBrowserBridge* raw = bridge.get();
  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  Result<std::unique_ptr<BrowserDevice>> device = std::move(request).take();
  ASSERT_THAT(device, HasResult());

  {
    std::unique_ptr<BrowserDevice> owned = std::move(device).result();
    ASSERT_THAT(owned->createBuffer(SimpleBuffer(BufferUsage::Vertex)), HasResult());
    ASSERT_THAT(owned->createTexture(SimpleTexture(TextureUsage::Sampled)), HasResult());
    EXPECT_THAT(raw->objectCount(), 2u);
  }

  EXPECT_THAT(raw->objectCount(), 0u);
}

TEST(BrowserDevice, RefusesEveryOperationFromAContextThatDoesNotOwnTheDevice) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  fixture.bridge->owned = false;

  EXPECT_THAT(fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("another worker")));
}

TEST(BrowserDevice, RefusesEveryOperationFromAnotherThread) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> fromOtherThread = Result<Buffer>(GpuError{GpuErrorType::InvalidState, "unset"});
  std::thread other(
      [&] { fromOtherThread = fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex)); });
  other.join();

  EXPECT_THAT(fromOtherThread, IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                     HasSubstr("cannot be used from another "
                                                               "thread")));
}

TEST(BrowserDevice, RefusesEveryOperationAfterTheDeviceIsLost) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());

  fixture.bridge->lost = true;
  fixture.bridge->lostReason = RcString("the browser reset the adapter");

  EXPECT_THAT(fixture.device->isDeviceLost(), true);
  EXPECT_THAT(fixture.device->deviceLostReason().str(), "the browser reset the adapter");
  EXPECT_THAT(fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex)),
              IsGpuErrorWithMessage(
                  GpuErrorType::InvalidState,
                  AllOf(HasSubstr("was lost"), HasSubstr("the browser reset the adapter"))));
}

TEST(BrowserDevice, SurfacesTheBrowsersRefusalAsAnIdentifierFailure) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  fixture.bridge->failOperation = "createBuffer";
  fixture.bridge->failStatus = BridgeStatus::UnknownObject;

  EXPECT_THAT(fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                    HasSubstr("no object under this identifier")));
  // A refused creation must leave nothing behind on either side, or the next resource in that
  // slot would be refused for a collision it did not cause.
  EXPECT_THAT(fixture.device->liveObjectCountForTest(), 0u);
  EXPECT_THAT(fixture.bridge->objectCount(), 0u);
}

TEST(BrowserDevice, SurfacesAKindMismatchAsAnIdentifierFailure) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  fixture.bridge->failOperation = "createTexture";
  fixture.bridge->failStatus = BridgeStatus::WrongObjectKind;

  EXPECT_THAT(
      fixture.device->createTexture(SimpleTexture(TextureUsage::Sampled)),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("object of another kind")));
}

TEST(BrowserDevice, MirrorsARecordedRenderPassOntoTheBridgeInRecordingOrder) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> target =
      fixture.device->createTexture(SimpleTexture(TextureUsage::RenderAttachment));
  ASSERT_THAT(target, HasResult());
  Result<TextureView> view =
      fixture.device->createTextureView(target.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());

  Result<PipelineLayout> pipelineLayout =
      fixture.device->createPipelineLayout(PipelineLayoutDescriptor{RcString("layout"), {}});
  ASSERT_THAT(pipelineLayout, HasResult());
  Result<ShaderModule> vertexModule =
      fixture.device->createShaderModule(SimpleShaderModule("vertex"));
  ASSERT_THAT(vertexModule, HasResult());
  Result<ShaderModule> fragmentModule =
      fixture.device->createShaderModule(SimpleShaderModule("fragment"));
  ASSERT_THAT(fragmentModule, HasResult());

  RenderPipelineDescriptor pipelineDescriptor;
  pipelineDescriptor.label = RcString("pipeline");
  pipelineDescriptor.layout = PipelineLayoutRef(pipelineLayout.result());
  pipelineDescriptor.vertex.module = ShaderModuleRef(vertexModule.result());
  pipelineDescriptor.vertex.entryPoint = RcString("vertexMain");
  pipelineDescriptor.fragment.module = ShaderModuleRef(fragmentModule.result());
  pipelineDescriptor.fragment.entryPoint = RcString("fragmentMain");
  pipelineDescriptor.fragment.targets.push_back(ColorTargetState{});
  Result<RenderPipeline> pipeline = fixture.device->createRenderPipeline(pipelineDescriptor);
  ASSERT_THAT(pipeline, HasResult());

  Result<std::unique_ptr<CommandEncoder>> encoder = fixture.device->createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  std::unique_ptr<CommandEncoder> commands = std::move(encoder).result();

  RenderPassDescriptor passDescriptor;
  passDescriptor.label = RcString("pass");
  passDescriptor.colorAttachments.push_back(RenderPassColorAttachment{
      TextureViewRef(view.result()), LoadOp::Clear, StoreOp::Store, {0.0, 0.0, 0.0, 1.0}});
  Result<RenderPassEncoder*> pass = commands->beginRenderPass(passDescriptor);
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->draw(3), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());

  Result<CommandBuffer> commandBuffer = commands->finish();
  ASSERT_THAT(commandBuffer, HasResult());

  const size_t beforeSubmit = fixture.bridge->calls.size();
  ASSERT_THAT(fixture.device->submit(std::move(commandBuffer).result()), HasResult());

  const std::vector<std::string> replayed(fixture.bridge->calls.begin() + beforeSubmit,
                                          fixture.bridge->calls.end());
  EXPECT_THAT(replayed,
              ElementsAre("beginCommandBuffer serial=1",
                          "beginRenderPass attachments=[(view=2 load=1 store=1 "
                          "clear=[0.000,0.000,0.000,1.000])]",
                          "setRenderPipeline pipeline=6",
                          "draw vertexCount=3 instanceCount=1 firstVertex=0 firstInstance=0",
                          "endRenderPass", "endCommandBuffer serial=1"));
}

TEST(BrowserDevice, ReportsTheSerialTheBrowserHasFinished) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  EXPECT_THAT(fixture.device->completedSerial(), 0u);
  fixture.bridge->completed = 7;
  EXPECT_THAT(fixture.device->completedSerial(), 7u);
}

TEST(BrowserDevice, MappingIsUnreadableUntilTheBrowserCompletesIt) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());

  Result<BufferMapping> mapping =
      fixture.device->mapBufferAsync(buffer.result(), MapMode::Read, 0, 4);
  ASSERT_THAT(mapping, HasResult());

  // Pending: a bounded wait spends its budget and says so rather than reporting a failure.
  EXPECT_THAT(fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 0.005}, {}),
              HasResult());
  EXPECT_THAT(fixture.device->mappedBytes(mapping.result()),
              IsGpuError(GpuErrorType::InvalidState));

  fixture.bridge->completeMapping(2, std::vector<uint8_t>{1, 2, 3, 4});
  Result<MapWaitOutcome> outcome =
      fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 0.05}, {});
  ASSERT_THAT(outcome, HasResult());
  EXPECT_THAT(outcome.result(), MapWaitOutcome::Ready);

  Result<std::span<const uint8_t>> bytes = fixture.device->mappedBytes(mapping.result());
  ASSERT_THAT(bytes, HasResult());
  EXPECT_THAT(std::vector<uint8_t>(bytes.result().begin(), bytes.result().end()),
              ElementsAre(1, 2, 3, 4));
}

TEST(BrowserDevice, LosingTheDeviceEndsAPendingMappingImmediately) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());
  Result<BufferMapping> mapping =
      fixture.device->mapBufferAsync(buffer.result(), MapMode::Read, 0, 4);
  ASSERT_THAT(mapping, HasResult());

  fixture.bridge->lost = true;

  Result<MapWaitOutcome> outcome =
      fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 10.0}, {});
  ASSERT_THAT(outcome, HasResult());
  EXPECT_THAT(outcome.result(), MapWaitOutcome::DeviceLost);
}

TEST(BrowserDevice, UnmappingMakesTheMappedBytesUnreachable) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());
  Result<BufferMapping> mapping =
      fixture.device->mapBufferAsync(buffer.result(), MapMode::Read, 0, 4);
  ASSERT_THAT(mapping, HasResult());
  fixture.bridge->completeMapping(2, std::vector<uint8_t>{9, 9, 9, 9});

  ASSERT_THAT(fixture.device->unmapBuffer(std::move(mapping).result()), IsOk());
  EXPECT_THAT(fixture.bridge->objectCount(), 1u);  // The buffer remains; the mapping is gone.
}

TEST(BrowserDevice, PresentsOnlyWhenTheBrowserChoosesTo) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  SurfaceDescriptor surfaceDescriptor;
  surfaceDescriptor.label = RcString("canvas");
  surfaceDescriptor.native.kind = NativeSurfaceKind::CanvasSelector;
  surfaceDescriptor.native.selector = RcString("#canvas");
  Result<Surface> surface = fixture.device->createSurface(surfaceDescriptor);
  ASSERT_THAT(surface, HasResult());

  SurfaceConfiguration configuration;
  configuration.format = TextureFormat::BGRA8Unorm;
  configuration.usage = TextureUsage::RenderAttachment;
  configuration.size = Extent2d{8, 8};
  configuration.alphaMode = SurfaceAlphaMode::Premultiplied;
  ASSERT_THAT(fixture.device->configureSurface(surface.result(), configuration), IsOk());

  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());
  EXPECT_THAT(acquired.result().status, SurfaceStatus::Success);

  EXPECT_THAT(fixture.device->presentSurface(surface.result()),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("own frame loop")));
}

TEST(BrowserDevice, AbandoningAFrameReleasesTheBrowserTextureBehindIt) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  SurfaceDescriptor surfaceDescriptor;
  surfaceDescriptor.native.kind = NativeSurfaceKind::CanvasSelector;
  surfaceDescriptor.native.selector = RcString("#canvas");
  Result<Surface> surface = fixture.device->createSurface(surfaceDescriptor);
  ASSERT_THAT(surface, HasResult());

  SurfaceConfiguration configuration;
  configuration.size = Extent2d{8, 8};
  ASSERT_THAT(fixture.device->configureSurface(surface.result(), configuration), IsOk());

  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());
  EXPECT_THAT(fixture.bridge->objectCount(), 2u);  // The surface plus the frame's texture.

  ASSERT_THAT(fixture.device->abandonCurrentTexture(surface.result()), IsOk());
  EXPECT_THAT(fixture.bridge->objectCount(), 1u);  // Only the surface remains.
}

TEST(BrowserDevice, AcquiringFromALostDeviceReportsTheLossAsASurfaceStatus) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  SurfaceDescriptor surfaceDescriptor;
  surfaceDescriptor.native.kind = NativeSurfaceKind::CanvasSelector;
  surfaceDescriptor.native.selector = RcString("#canvas");
  Result<Surface> surface = fixture.device->createSurface(surfaceDescriptor);
  ASSERT_THAT(surface, HasResult());

  SurfaceConfiguration configuration;
  configuration.size = Extent2d{8, 8};
  ASSERT_THAT(fixture.device->configureSurface(surface.result(), configuration), IsOk());

  fixture.bridge->lost = true;

  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());
  EXPECT_THAT(acquired.result().status, SurfaceStatus::DeviceLost);
}

TEST(BrowserDevice, RefusesASurfaceThatDoesNotNameACanvas) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  int layer = 0;
  SurfaceDescriptor surfaceDescriptor;
  surfaceDescriptor.native.kind = NativeSurfaceKind::MetalLayer;
  surfaceDescriptor.native.display = &layer;
  EXPECT_THAT(fixture.device->createSurface(surfaceDescriptor),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("CSS selector")));
}

TEST(BrowserDevice, DecodesOnlyTheSurfaceCapabilitiesItRecognizes) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  // The second format code is one this protocol has no meaning for: the browser is describing
  // itself, so an unrecognized value is dropped rather than cast into an enumerator.
  fixture.bridge->capabilities.formatCodes = {2, 4000};
  fixture.bridge->capabilities.usageBits = 1;
  fixture.bridge->capabilities.presentModeCodes = {1};
  fixture.bridge->capabilities.alphaModeCodes = {2};

  SurfaceDescriptor surfaceDescriptor;
  surfaceDescriptor.native.kind = NativeSurfaceKind::CanvasSelector;
  surfaceDescriptor.native.selector = RcString("#canvas");
  Result<Surface> surface = fixture.device->createSurface(surfaceDescriptor);
  ASSERT_THAT(surface, HasResult());

  Result<SurfaceCapabilities> capabilities = fixture.device->surfaceCapabilities(surface.result());
  ASSERT_THAT(capabilities, HasResult());
  EXPECT_THAT(capabilities.result().formats, ElementsAre(TextureFormat::BGRA8Unorm));
  EXPECT_THAT(capabilities.result().usages, TextureUsage::RenderAttachment);
  EXPECT_THAT(capabilities.result().presentModes, ElementsAre(PresentMode::Fifo));
  EXPECT_THAT(capabilities.result().alphaModes, ElementsAre(SurfaceAlphaMode::Premultiplied));
}

TEST(BrowserDevice, AcceptsOnlyTheWgslProjection) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  EXPECT_THAT(fixture.device->shaderSourceKind(), ShaderSourceKind::Wgsl);

  ShaderModuleDescriptor descriptor;
  descriptor.label = RcString("msl");
  descriptor.sourceText = RcString("kernel void main() {}");
  descriptor.sourceKind = ShaderSourceKind::Msl;
  EXPECT_THAT(fixture.device->createShaderModule(descriptor),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("WGSL projection")));
}

TEST(BrowserDevice, WritesTexelRowsThroughTheBridge) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> texture = fixture.device->createTexture(SimpleTexture(TextureUsage::CopyDst));
  ASSERT_THAT(texture, HasResult());

  const std::vector<uint8_t> payload(256 * 4, 0u);
  const TexelCopyBufferLayout layout{0, 256, 4};
  ASSERT_THAT(fixture.device->writeTexture(texture.result(), payload, layout, Extent2d{4, 4}),
              IsOk());

  EXPECT_THAT(fixture.bridge->calls.back(),
              "writeTexture texture=1 bytes=1024 offset=0 bytesPerRow=256 rowsPerImage=4 "
              "destination=(0,0) size=4x4");
}

}  // namespace donner::gpu::browser
