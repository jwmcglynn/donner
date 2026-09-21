#include "donner/gpu/browser/BrowserDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
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
using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::Not;

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

/// Records a command buffer holding one clear-only render pass targeting \p view.
/// @param device Device to record against. @param view Attachment view.
CommandBuffer RecordClearPass(Device& device, const TextureView& view) {
  Result<std::unique_ptr<CommandEncoder>> encoder = device.createCommandEncoder();
  EXPECT_THAT(encoder, HasResult());
  std::unique_ptr<CommandEncoder> commands = std::move(encoder).result();

  RenderPassDescriptor passDescriptor;
  passDescriptor.colorAttachments.push_back(RenderPassColorAttachment{
      TextureViewRef(view), LoadOp::Clear, StoreOp::Store, {0.0, 0.0, 0.0, 1.0}});
  Result<RenderPassEncoder*> pass = commands->beginRenderPass(passDescriptor);
  EXPECT_THAT(pass, HasResult());
  if (pass.hasResult()) {
    EXPECT_THAT(pass.result()->end(), IsOk());
  }

  Result<CommandBuffer> commandBuffer = commands->finish();
  EXPECT_THAT(commandBuffer, HasResult());
  if (commandBuffer.hasError()) {
    return CommandBuffer();
  }
  return std::move(commandBuffer).result();
}

/// A descriptor naming the canvas a browser surface presents to.
SurfaceDescriptor CanvasSurface() {
  SurfaceDescriptor descriptor;
  descriptor.label = RcString("canvas");
  descriptor.native.kind = NativeSurfaceKind::CanvasSelector;
  descriptor.native.selector = RcString("#canvas");
  return descriptor;
}

/// A configuration a canvas surface accepts. @param size Extent to present at.
SurfaceConfiguration CanvasConfiguration(Extent2d size) {
  SurfaceConfiguration configuration;
  configuration.format = TextureFormat::BGRA8Unorm;
  configuration.usage = TextureUsage::RenderAttachment;
  configuration.size = size;
  configuration.alphaMode = SurfaceAlphaMode::Premultiplied;
  return configuration;
}

/// The lines of \p calls belonging to a surface, in order.
///
/// Named operation by operation rather than by searching the whole line, so a recorded argument
/// that happens to carry one of these words cannot join the sequence. What comes back is the
/// presentation sequence on its own: what a test of the frame contract asserts, rather than the
/// presence of one line somewhere in the whole stream.
/// @param calls Recorded lines.
std::vector<std::string> SurfaceCalls(const std::vector<std::string>& calls) {
  static constexpr std::string_view kSurfaceOperations[] = {
      "createSurface ", "configureSurface ", "acquireCurrentTexture ", "abandonCurrentTexture ",
      "destroyObject kind=surface "};

  std::vector<std::string> selected;
  for (const std::string& line : calls) {
    for (const std::string_view operation : kSurfaceOperations) {
      if (line.starts_with(operation)) {
        selected.push_back(line);
        break;
      }
    }
  }
  return selected;
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

  EXPECT_THAT(*fixture.bridge->calls,
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
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  // The registry outlives the bridge, which the device owns: what teardown released has to stay
  // observable after the device that did the releasing is gone.
  std::shared_ptr<std::map<BrowserObjectId, BrowserObjectKind>> objects = fixture.bridge->objects;

  // These handles deliberately outlive the device. A handle that does releases nothing, so this
  // measures the device's own teardown rather than the handles unwinding first.
  Result<Buffer> buffer = fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex));
  ASSERT_THAT(buffer, HasResult());
  Result<Texture> texture = fixture.device->createTexture(SimpleTexture(TextureUsage::Sampled));
  ASSERT_THAT(texture, HasResult());
  EXPECT_THAT(objects->size(), 2u);

  fixture.device.reset();
  EXPECT_THAT(objects->size(), 0u);
}

TEST(BrowserDevice, RefusesCreationFromAContextThatDoesNotOwnTheDevice) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  fixture.bridge->owned = false;

  EXPECT_THAT(fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("another worker")));
}

TEST(BrowserDevice, RefusesCreationFromAnotherThread) {
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

TEST(BrowserDevice, RefusesCreationAfterTheDeviceIsLost) {
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

TEST(BrowserDevice, StillFreesItsBrowserObjectsAfterTheDeviceIsLost) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer = fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex));
  ASSERT_THAT(buffer, HasResult());
  EXPECT_THAT(fixture.bridge->objectCount(), 1u);

  fixture.bridge->lost = true;

  // Loss refuses what can be refused, but not a release: a lost device that kept its objects would
  // strand them for the life of the page.
  EXPECT_THAT(fixture.device->destroyBuffer(std::move(buffer).result()), IsOk());
  EXPECT_THAT(fixture.bridge->objectCount(), 0u);
}

TEST(BrowserDevice, DoesNotReleaseBrowserObjectsFromAThreadThatDoesNotOwnTheDevice) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer = fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex));
  ASSERT_THAT(buffer, HasResult());
  ASSERT_THAT(fixture.bridge->objectCount(), 1u);

  // Dropping the handle elsewhere would otherwise name the object to a worker that does not hold
  // it, which releases nothing and loses the identifier.
  std::thread other([&] { Buffer released = std::move(buffer).result(); });
  other.join();

  EXPECT_THAT(fixture.device->foreignThreadReleasesForTest(), 1u);
  EXPECT_THAT(fixture.bridge->objectCount(), 1u);
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Buffer, 1), true);

  // The entry is kept, so teardown on the owning thread still frees it.
  std::shared_ptr<std::map<BrowserObjectId, BrowserObjectKind>> objects = fixture.bridge->objects;
  fixture.device.reset();
  EXPECT_THAT(objects->size(), 0u);
}

TEST(BrowserDevice, DiscardsARecordingLeftOpenByARefusedSubmission) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> target =
      fixture.device->createTexture(SimpleTexture(TextureUsage::RenderAttachment));
  ASSERT_THAT(target, HasResult());
  Result<TextureView> view =
      fixture.device->createTextureView(target.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());

  // The browser refuses the pass, so the submission stops with a recording still open.
  fixture.bridge->failOperation = "beginRenderPass";
  fixture.bridge->failStatus = BridgeStatus::Failed;
  ASSERT_THAT(fixture.device->submit(RecordClearPass(*fixture.device, view.result())),
              IsGpuError(GpuErrorType::InvalidState));

  // The next submission opens a fresh recording rather than continuing that one, so nothing
  // recorded before the refusal can reach the queue.
  fixture.bridge->failOperation.clear();
  const size_t beforeSubmit = fixture.bridge->calls->size();
  ASSERT_THAT(fixture.device->submit(RecordClearPass(*fixture.device, view.result())), HasResult());

  const std::vector<std::string> replayed(fixture.bridge->calls->begin() + beforeSubmit,
                                          fixture.bridge->calls->end());
  EXPECT_THAT(replayed, ElementsAre("beginCommandBuffer serial=1 index=0",
                                    "beginRenderPass attachments=[(view=2 load=1 store=1 "
                                    "clear=[0.000,0.000,0.000,1.000])]",
                                    "endRenderPass", "endCommandBuffer serial=1",
                                    "submitCommandBuffers serial=1 count=1"));
}

TEST(BrowserDevice, RecordsEveryBufferOfASpanAndSubmitsThemTogether) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> target =
      fixture.device->createTexture(SimpleTexture(TextureUsage::RenderAttachment));
  ASSERT_THAT(target, HasResult());
  Result<TextureView> view =
      fixture.device->createTextureView(target.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());

  std::array<CommandBuffer, 2> span{RecordClearPass(*fixture.device, view.result()),
                                    RecordClearPass(*fixture.device, view.result())};
  const size_t beforeSubmit = fixture.bridge->calls->size();
  ASSERT_THAT(fixture.device->submit(span), HasResult());

  // Each buffer is recorded through its own browser encoder under the one serial, and the queue
  // is reached once, with both of them, in recording order.
  const std::vector<std::string> replayed(fixture.bridge->calls->begin() + beforeSubmit,
                                          fixture.bridge->calls->end());
  EXPECT_THAT(replayed, ElementsAre("beginCommandBuffer serial=1 index=0",
                                    "beginRenderPass attachments=[(view=2 load=1 store=1 "
                                    "clear=[0.000,0.000,0.000,1.000])]",
                                    "endRenderPass", "endCommandBuffer serial=1",
                                    "beginCommandBuffer serial=1 index=1",
                                    "beginRenderPass attachments=[(view=2 load=1 store=1 "
                                    "clear=[0.000,0.000,0.000,1.000])]",
                                    "endRenderPass", "endCommandBuffer serial=1",
                                    "submitCommandBuffers serial=1 count=2"));
}

TEST(BrowserDevice, DropsTheFinishedBuffersOfASpanRefusedPartway) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> target =
      fixture.device->createTexture(SimpleTexture(TextureUsage::RenderAttachment));
  ASSERT_THAT(target, HasResult());
  Result<TextureView> view =
      fixture.device->createTextureView(target.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());

  // The browser refuses the second buffer's pass, so the submission stops with the first buffer
  // already finished on the browser side.
  fixture.bridge->failOperation = "beginCommandBuffer serial=1 index=1";
  fixture.bridge->failStatus = BridgeStatus::Failed;
  std::array<CommandBuffer, 2> refused{RecordClearPass(*fixture.device, view.result()),
                                       RecordClearPass(*fixture.device, view.result())};
  ASSERT_THAT(fixture.device->submit(refused), IsGpuError(GpuErrorType::InvalidState));

  // A refused submission keeps its serial, so the retry arrives under the same one and must
  // start its list over rather than submitting the buffer the refused attempt left finished.
  fixture.bridge->failOperation.clear();
  std::array<CommandBuffer, 1> retried{RecordClearPass(*fixture.device, view.result())};
  const size_t beforeRetry = fixture.bridge->calls->size();
  ASSERT_THAT(fixture.device->submit(retried), HasResult());

  const std::vector<std::string> replayed(fixture.bridge->calls->begin() + beforeRetry,
                                          fixture.bridge->calls->end());
  EXPECT_THAT(replayed, ElementsAre("beginCommandBuffer serial=1 index=0",
                                    "beginRenderPass attachments=[(view=2 load=1 store=1 "
                                    "clear=[0.000,0.000,0.000,1.000])]",
                                    "endRenderPass", "endCommandBuffer serial=1",
                                    "submitCommandBuffers serial=1 count=1"));
}

TEST(BrowserDevice, ClosesARecordingWithTheSerialThatOpenedIt) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> target =
      fixture.device->createTexture(SimpleTexture(TextureUsage::RenderAttachment));
  ASSERT_THAT(target, HasResult());
  Result<TextureView> view =
      fixture.device->createTextureView(target.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());

  // Two submissions in a row: each has to open and close under its own serial, which is what tells
  // the browser side the encoder it is finishing is the one it was given.
  ASSERT_THAT(fixture.device->submit(RecordClearPass(*fixture.device, view.result())), HasResult());
  const size_t beforeSecond = fixture.bridge->calls->size();
  ASSERT_THAT(fixture.device->submit(RecordClearPass(*fixture.device, view.result())), HasResult());

  const std::vector<std::string> replayed(fixture.bridge->calls->begin() + beforeSecond,
                                          fixture.bridge->calls->end());
  EXPECT_THAT(replayed.front(), "beginCommandBuffer serial=2 index=0");
  EXPECT_THAT(replayed.back(), "submitCommandBuffers serial=2 count=1");
}

TEST(BrowserDevice, MapsBuffersForHostReadsOnly) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());

  EXPECT_THAT(fixture.device->mapBufferAsync(buffer.result(), static_cast<MapMode>(7), 0, 4),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("host reads only")));
}

TEST(BrowserDevice, HandsAFrameBackRatherThanDestroyingItWhenTheDeviceIsDestroyed) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::map<BrowserObjectId, BrowserObjectKind>> objects = fixture.bridge->objects;
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());

  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());
  ASSERT_THAT(objects->size(), 2u);

  // A frame texture belongs to the canvas that supplied it, so teardown gives it back through the
  // surface rather than destroying it.
  fixture.device.reset();
  EXPECT_THAT(objects->size(), 0u);
  EXPECT_THAT(*calls, Contains("abandonCurrentTexture surface=1"));
  EXPECT_THAT(*calls, Not(Contains("destroyObject kind=texture id=2")));
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

  const size_t beforeSubmit = fixture.bridge->calls->size();
  ASSERT_THAT(fixture.device->submit(std::move(commandBuffer).result()), HasResult());

  const std::vector<std::string> replayed(fixture.bridge->calls->begin() + beforeSubmit,
                                          fixture.bridge->calls->end());
  EXPECT_THAT(replayed,
              ElementsAre("beginCommandBuffer serial=1 index=0",
                          "beginRenderPass attachments=[(view=2 load=1 store=1 "
                          "clear=[0.000,0.000,0.000,1.000])]",
                          "setRenderPipeline pipeline=6",
                          "draw vertexCount=3 instanceCount=1 firstVertex=0 firstInstance=0",
                          "endRenderPass", "endCommandBuffer serial=1",
                          "submitCommandBuffers serial=1 count=1"));
}

TEST(BrowserDevice, MirrorsARecordedComputePassOntoTheBridgeInRecordingOrder) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> storage = fixture.device->createBuffer(SimpleBuffer(BufferUsage::Storage));
  ASSERT_THAT(storage, HasResult());

  BindGroupLayoutDescriptor layoutDescriptor;
  layoutDescriptor.label = RcString("computeLayout");
  layoutDescriptor.entries.push_back(BindGroupLayoutEntry{
      0, ShaderStage::Compute, BindingType::ReadOnlyStorageBuffer, TextureFormat::RGBA8Unorm});
  Result<BindGroupLayout> bindGroupLayout = fixture.device->createBindGroupLayout(layoutDescriptor);
  ASSERT_THAT(bindGroupLayout, HasResult());

  BindGroupDescriptor groupDescriptor;
  groupDescriptor.label = RcString("computeGroup");
  groupDescriptor.layout = BindGroupLayoutRef(bindGroupLayout.result());
  groupDescriptor.entries.push_back(
      BindGroupEntry{0, BufferBinding{BufferRef(storage.result()), 0, 256}});
  Result<BindGroup> bindGroup = fixture.device->createBindGroup(groupDescriptor);
  ASSERT_THAT(bindGroup, HasResult());

  PipelineLayoutDescriptor pipelineLayoutDescriptor;
  pipelineLayoutDescriptor.label = RcString("computePipelineLayout");
  pipelineLayoutDescriptor.bindGroupLayouts.push_back(BindGroupLayoutRef(bindGroupLayout.result()));
  Result<PipelineLayout> pipelineLayout =
      fixture.device->createPipelineLayout(pipelineLayoutDescriptor);
  ASSERT_THAT(pipelineLayout, HasResult());

  ShaderModuleDescriptor moduleDescriptor = SimpleShaderModule("compute");
  moduleDescriptor.computeEntryPoints.push_back(
      ComputeEntryPointInfo{RcString("computeMain"), WorkgroupSize{8, 8, 1}});
  Result<ShaderModule> computeModule = fixture.device->createShaderModule(moduleDescriptor);
  ASSERT_THAT(computeModule, HasResult());

  ComputePipelineDescriptor pipelineDescriptor;
  pipelineDescriptor.label = RcString("computePipeline");
  pipelineDescriptor.layout = PipelineLayoutRef(pipelineLayout.result());
  pipelineDescriptor.compute.module = ShaderModuleRef(computeModule.result());
  pipelineDescriptor.compute.entryPoint = RcString("computeMain");
  pipelineDescriptor.workgroupSize = WorkgroupSize{8, 8, 1};
  Result<ComputePipeline> pipeline = fixture.device->createComputePipeline(pipelineDescriptor);
  ASSERT_THAT(pipeline, HasResult());

  Result<std::unique_ptr<CommandEncoder>> encoder = fixture.device->createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  std::unique_ptr<CommandEncoder> commands = std::move(encoder).result();

  Result<ComputePassEncoder*> pass = commands->beginComputePass(ComputePassDescriptor{});
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setBindGroup(0, bindGroup.result()), IsOk());
  ASSERT_THAT(pass.result()->dispatchWorkgroups(2, 3, 1), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());

  Result<CommandBuffer> commandBuffer = commands->finish();
  ASSERT_THAT(commandBuffer, HasResult());

  const size_t beforeSubmit = fixture.bridge->calls->size();
  ASSERT_THAT(fixture.device->submit(std::move(commandBuffer).result()), HasResult());

  const std::vector<std::string> replayed(fixture.bridge->calls->begin() + beforeSubmit,
                                          fixture.bridge->calls->end());
  EXPECT_THAT(replayed,
              ElementsAre("beginCommandBuffer serial=1 index=0", "beginComputePass",
                          "setComputePipeline pipeline=6", "setBindGroup index=0 bindGroup=3",
                          "dispatchWorkgroups count=2x3x1", "endComputePass",
                          "endCommandBuffer serial=1", "submitCommandBuffers serial=1 count=1"));
}

TEST(BrowserDevice, MirrorsRecordedCopiesOntoTheBridge) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> source = fixture.device->createTexture(SimpleTexture(TextureUsage::CopySrc));
  ASSERT_THAT(source, HasResult());
  Result<Texture> destination = fixture.device->createTexture(SimpleTexture(TextureUsage::CopyDst));
  ASSERT_THAT(destination, HasResult());

  BufferDescriptor readbackDescriptor = SimpleBuffer(BufferUsage::CopyDst);
  readbackDescriptor.byteSize = 1024;
  Result<Buffer> readback = fixture.device->createBuffer(readbackDescriptor);
  ASSERT_THAT(readback, HasResult());

  Result<std::unique_ptr<CommandEncoder>> encoder = fixture.device->createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  std::unique_ptr<CommandEncoder> commands = std::move(encoder).result();

  ASSERT_THAT(commands->copyTextureToBuffer(TexelCopyTextureInfo{TextureRef(source.result())},
                                            readback.result(), TexelCopyBufferLayout{0, 256, 4},
                                            Extent2d{4, 4}),
              IsOk());
  ASSERT_THAT(commands->copyTextureToTexture(source.result(), destination.result(), Extent2d{2, 2},
                                             Origin2d{1, 1}, Origin2d{0, 2}),
              IsOk());

  Result<CommandBuffer> commandBuffer = commands->finish();
  ASSERT_THAT(commandBuffer, HasResult());

  const size_t beforeSubmit = fixture.bridge->calls->size();
  ASSERT_THAT(fixture.device->submit(std::move(commandBuffer).result()), HasResult());

  const std::vector<std::string> replayed(fixture.bridge->calls->begin() + beforeSubmit,
                                          fixture.bridge->calls->end());
  EXPECT_THAT(replayed,
              ElementsAre("beginCommandBuffer serial=1 index=0",
                          "copyTextureToBuffer texture=1 buffer=3 offset=0 bytesPerRow=256 "
                          "rowsPerImage=4 size=4x4",
                          "copyTextureToTexture source=1 destination=2 sourceOrigin=(1,1) "
                          "destinationOrigin=(0,2) size=2x2",
                          "endCommandBuffer serial=1", "submitCommandBuffers serial=1 count=1"));
}

TEST(BrowserDevice, ReportsTheSerialTheBrowserHasFinished) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  EXPECT_THAT(fixture.device->completedSerial(), 0u);
  fixture.bridge->completed = 7;
  EXPECT_THAT(fixture.device->completedSerial(), 7u);
}

TEST(BrowserDevice, WaitingForASerialGivesTheBrowserTheThreadUntilItReportsTheWorkDone) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  // Nothing completes while this thread holds the event loop, so a wait that only rested would
  // spend its whole budget without the browser ever getting the chance to finish the work.
  fixture.bridge->onYield = [&] { fixture.bridge->completed = 5; };

  EXPECT_THAT(fixture.device->waitForSerial(5, 10.0), testing::IsTrue());
  EXPECT_THAT(fixture.bridge->yieldCount, 1u);
}

TEST(BrowserDevice, WaitingForAnAlreadyFinishedSerialNeedsNoYield) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  fixture.bridge->completed = 5;

  EXPECT_THAT(fixture.device->waitForSerial(5, 10.0), testing::IsTrue());
  EXPECT_THAT(fixture.bridge->yieldCount, 0u);
}

TEST(BrowserDevice, WaitingForASerialOnALostDeviceGivesUpAtOnce) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  fixture.bridge->lost = true;

  // A budget large enough that spending it would hang the test rather than fail it.
  EXPECT_THAT(fixture.device->waitForSerial(5, 3600.0), testing::IsFalse());
  EXPECT_THAT(fixture.bridge->yieldCount, 0u)
      << "a lost device can never finish the work, so the browser must not be handed the thread";
}

TEST(BrowserDevice, RefusesASerialWaitEnteredFromInsideItsOwnYield) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  // Handing the thread over a second time would stack one unwind on another, which the runtime
  // underneath cannot represent.
  bool nested = true;
  fixture.bridge->onYield = [&] {
    nested = fixture.device->waitForSerial(5, 1.0);
    fixture.bridge->completed = 5;
  };

  EXPECT_THAT(fixture.device->waitForSerial(5, 10.0), testing::IsTrue());
  EXPECT_THAT(nested, testing::IsFalse());
  EXPECT_THAT(fixture.device->nestedWaitRefusalsForTest(), 1u);
}

TEST(BrowserDevice, DestroyingABackingReleasesTheBrowserObjectAndFailsClosedOnAStaleHandle) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  const std::shared_ptr<std::map<BrowserObjectId, BrowserObjectKind>> objects =
      fixture.bridge->objects;

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());
  Result<Texture> texture = fixture.device->createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc});
  ASSERT_THAT(texture, HasResult());
  const uint32_t textureSlotIndex = texture.result().slotIndex();
  const uint32_t textureGeneration = texture.result().generation();
  const uint64_t deviceId = texture.result().deviceId();
  ASSERT_THAT(objects->size(), 2u);

  EXPECT_THAT(fixture.device->destroyBufferBacking(std::move(buffer).result()), IsOk());
  EXPECT_THAT(fixture.device->destroyTextureBacking(std::move(texture).result()), IsOk());
  EXPECT_THAT(objects->size(), 0u);

  // The freed slot is handed to the next texture, which is what the stale handle must not reach.
  Result<Texture> replacement = fixture.device->createTexture(TextureDescriptor{
      "replacement", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc});
  ASSERT_THAT(replacement, HasResult());
  ASSERT_THAT(replacement.result().slotIndex(), textureSlotIndex);

  EXPECT_THAT(fixture.device->destroyTextureBacking(
                  Texture::CreateForBackend(textureSlotIndex, textureGeneration, deviceId)),
              IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(objects->size(), 1u) << "a stale handle must not release the slot's new occupant";
  EXPECT_THAT(fixture.device->ownsTextureBacking(replacement.result()), testing::IsTrue());
}

TEST(BrowserDevice, OwnsTheBackingOfATextureItAllocatedButNotOfAHandleThatNamesNothingHere) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> texture = fixture.device->createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc});
  ASSERT_THAT(texture, HasResult());

  EXPECT_THAT(fixture.device->ownsTextureBacking(texture.result()), testing::IsTrue());
  EXPECT_THAT(fixture.device->ownsTextureBacking(Texture()), testing::IsFalse());
  EXPECT_THAT(fixture.device->ownsTextureBacking(Texture::CreateForBackend(
                  texture.result().slotIndex(), texture.result().generation(),
                  texture.result().deviceId() + 1)),
              testing::IsFalse())
      << "a handle from another device names nothing here";
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
  Result<MapWaitReport> outcome =
      fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 0.05}, {});
  ASSERT_THAT(outcome, HasResult());
  EXPECT_THAT(outcome.result().outcome, MapWaitOutcome::Ready);

  Result<std::span<const uint8_t>> bytes = fixture.device->mappedBytes(mapping.result());
  ASSERT_THAT(bytes, HasResult());
  EXPECT_THAT(std::vector<uint8_t>(bytes.result().begin(), bytes.result().end()),
              ElementsAre(1, 2, 3, 4));
}

TEST(BrowserDevice, GivesTheBrowserTheThreadWhileAMappingIsPending) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());
  Result<BufferMapping> mapping =
      fixture.device->mapBufferAsync(buffer.result(), MapMode::Read, 0, 4);
  ASSERT_THAT(mapping, HasResult());

  // A browser settles a mapping from its event loop, so the wait has to hand the thread over. The
  // bridge stands in for that: the mapping completes only while the device is yielding, which is
  // exactly the progress a wait that merely rested would never allow.
  fixture.bridge->onYield = [&] { fixture.bridge->completeMapping(2, std::vector<uint8_t>{7}); };

  Result<MapWaitReport> outcome =
      fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 0.05}, {});
  ASSERT_THAT(outcome, HasResult());
  EXPECT_THAT(outcome.result().outcome, MapWaitOutcome::Ready);
  EXPECT_THAT(fixture.bridge->yieldCount, 1u);
}

TEST(BrowserDevice, BoundsTheSliceItHandsToTheBrowser) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());
  Result<BufferMapping> mapping =
      fixture.device->mapBufferAsync(buffer.result(), MapMode::Read, 0, 4);
  ASSERT_THAT(mapping, HasResult());

  // The runtime only checks that a slice is above zero, so a caller can ask for one no fixed-width
  // unit could hold. Complete the mapping from the yield so the wait ends after one slice.
  fixture.bridge->onYield = [&] { fixture.bridge->completeMapping(2, std::vector<uint8_t>{1}); };
  Result<MapWaitReport> outcome =
      fixture.device->waitForMapping(mapping.result(), MapWaitParams{1.0e12, 1.0e12}, {});
  ASSERT_THAT(outcome, HasResult());
  EXPECT_THAT(outcome.result().outcome, MapWaitOutcome::Ready);

  // Bounded, and the bound does not shorten the wait: the runtime re-enters until its own budget
  // elapses, so a clamped slice only hands the thread back more often.
  EXPECT_THAT(fixture.bridge->yieldCount, 1u);
  EXPECT_THAT(fixture.bridge->yieldedSeconds, testing::Le(1.0));
  EXPECT_THAT(fixture.bridge->yieldedSeconds, testing::Gt(0.0));
}

TEST(BrowserDevice, RefusesAWaitEnteredFromInsideItsOwnYield) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());
  Result<BufferMapping> mapping =
      fixture.device->mapBufferAsync(buffer.result(), MapMode::Read, 0, 4);
  ASSERT_THAT(mapping, HasResult());

  // Waiting again from inside the yield is what a callback running on the browser's thread could
  // do. Handing the thread over a second time would stack one unwind on another, which the runtime
  // underneath cannot represent, so the nested wait is refused rather than taken.
  Result<MapWaitReport> nested = Result<MapWaitReport>(GpuError{GpuErrorType::InvalidState, ""});
  fixture.bridge->onYield = [&] {
    nested = fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 0.01}, {});
    fixture.bridge->completeMapping(2, std::vector<uint8_t>{1});
  };

  Result<MapWaitReport> outcome =
      fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 0.05}, {});
  ASSERT_THAT(outcome, HasResult());
  EXPECT_THAT(outcome.result().outcome, MapWaitOutcome::Ready);

  ASSERT_THAT(nested, HasResult());
  EXPECT_THAT(nested.result().outcome, MapWaitOutcome::Failed);
  EXPECT_THAT(fixture.device->nestedWaitRefusalsForTest(), 1u);
  // Only the outer wait handed the thread over; the refused one did not.
  EXPECT_THAT(fixture.bridge->yieldCount, 1u);
}

TEST(BrowserDevice, DoesNotYieldOnceAMappingHasSettled) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Buffer> buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());
  Result<BufferMapping> mapping =
      fixture.device->mapBufferAsync(buffer.result(), MapMode::Read, 0, 4);
  ASSERT_THAT(mapping, HasResult());
  fixture.bridge->completeMapping(2, std::vector<uint8_t>{7});

  // Already readable, so there is nothing to wait for and no reason to give up the thread.
  Result<MapWaitReport> outcome =
      fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 0.05}, {});
  ASSERT_THAT(outcome, HasResult());
  EXPECT_THAT(outcome.result().outcome, MapWaitOutcome::Ready);
  EXPECT_THAT(fixture.bridge->yieldCount, 0u);
}

TEST(BrowserDevice, ASurfaceTakingTheSlotOfADestroyedOneStartsWithNoFrame) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  // Destroying the surface hands its frame back, so the slot the next surface takes carries no
  // record of one; what this covers is that the replacement starts from nothing either way.
  ASSERT_THAT(fixture.device->destroySurface(std::move(surface).result()), IsOk());
  Result<Surface> replacement = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(replacement, HasResult());

  // The frame went back to the surface that supplied it rather than being left named by a slot
  // that now belongs to a different surface.
  EXPECT_THAT(*calls, Contains("abandonCurrentTexture surface=1"));
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 2), false);
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

  Result<MapWaitReport> outcome =
      fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 10.0}, {});
  ASSERT_THAT(outcome, HasResult());
  EXPECT_THAT(outcome.result().outcome, MapWaitOutcome::DeviceLost);
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

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());

  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());
  EXPECT_THAT(acquired.result().status, SurfaceStatus::Success);

  EXPECT_THAT(fixture.device->presentSurface(surface.result()),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("own frame loop")));
}

TEST(BrowserDevice, AbandoningAFrameReleasesTheBrowserTextureBehindIt) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());

  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());
  EXPECT_THAT(fixture.bridge->objectCount(), 2u);  // The surface plus the frame's texture.

  ASSERT_THAT(fixture.device->abandonCurrentTexture(surface.result()), IsOk());
  EXPECT_THAT(fixture.bridge->objectCount(), 1u);  // Only the surface remains.
}

TEST(BrowserDevice, ATextureTakingTheSlotOfAFrameGetsItsOwnIdentifier) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::map<BrowserObjectId, BrowserObjectKind>> objects = fixture.bridge->objects;
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());

  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());
  ASSERT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 2), true);

  // Destroying the surface hands its frame back and releases the texture slot the frame occupied,
  // so that slot is free for the next texture the caller creates.
  ASSERT_THAT(fixture.device->destroySurface(std::move(surface).result()), IsOk());

  // The next texture takes the released slot, and the identifier it is given is a new one: an
  // identifier is never reused, so nothing can name the frame that occupied the slot before it.
  Result<Texture> reused = fixture.device->createTexture(SimpleTexture(TextureUsage::Sampled));
  ASSERT_THAT(reused, HasResult());
  EXPECT_THAT(*calls, Contains("abandonCurrentTexture surface=1"));
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 2), false);
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 3), true);

  // Teardown destroys the caller's texture and does not reach the frame, which the canvas owns.
  fixture.device.reset();
  EXPECT_THAT(objects->size(), 0u);
  EXPECT_THAT(*calls, Contains("destroyObject kind=texture id=3"));
  EXPECT_THAT(*calls, Not(Contains("destroyObject kind=texture id=2")));
}

TEST(BrowserDeviceRequest, CarriesTheBrowsersReasonOutOfAFailedBegin) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  bridge->beginStatus = BridgeStatus::Failed;
  bridge->requestError = RcString("protocol entry 7 is 4 and this library assigns 8");

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.state(), BrowserDeviceRequestState::Failed);
  // The detail is the diagnosis; flattening it into the generic refusal would lose which entry
  // disagreed.
  EXPECT_THAT(request.error().str(), HasSubstr("protocol entry 7"));
}

TEST(BrowserDevice, AcquiringFromALostDeviceReportsTheLossAsASurfaceStatus) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());

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

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  Result<SurfaceCapabilities> capabilities = fixture.device->surfaceCapabilities(surface.result());
  ASSERT_THAT(capabilities, HasResult());
  EXPECT_THAT(capabilities.result().formats, ElementsAre(TextureFormat::BGRA8Unorm));
  EXPECT_THAT(capabilities.result().usages, TextureUsage::RenderAttachment);
  EXPECT_THAT(capabilities.result().presentModes, ElementsAre(PresentMode::Fifo));
  EXPECT_THAT(capabilities.result().alphaModes, ElementsAre(SurfaceAlphaMode::Premultiplied));
}

TEST(BrowserDevice, AcquiringBeforeConfiguringReachesNoCanvas) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  // A canvas whose context has never been configured has no frame to give, so the refusal has to
  // come before the browser is asked rather than from the browser refusing.
  EXPECT_THAT(fixture.device->acquireCurrentTexture(surface.result()),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("not been configured")));
  EXPECT_THAT(SurfaceCalls(*calls), ElementsAre("createSurface id=1 canvas=#canvas"));
  EXPECT_THAT(fixture.bridge->objectCount(), 1u);
}

TEST(BrowserDevice, AcquiringTwiceInOneFrameMintsNoSecondFrame) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  // A canvas holds one frame at a time, so a second acquisition inside the same frame is refused
  // without a second identifier being minted for a frame the canvas never handed over.
  EXPECT_THAT(fixture.device->acquireCurrentTexture(surface.result()),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("not been presented")));
  EXPECT_THAT(SurfaceCalls(*calls),
              ElementsAre("createSurface id=1 canvas=#canvas",
                          "configureSurface surface=1 format=2 usage=1 size=8x8 alphaMode=2",
                          "acquireCurrentTexture surface=1 texture=2"));
  EXPECT_THAT(fixture.bridge->objectCount(), 2u);
}

TEST(BrowserDevice, ReconfiguringASurfaceEndsTheFrameItHadAcquired) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());
  ASSERT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 2), true);

  // Reconfiguring is how a canvas follows its element's size, and it replaces the swap chain
  // behind the context: the outstanding frame goes back before the new configuration is applied,
  // and the next acquisition is a different frame rather than the same one renamed.
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{16, 16})),
      IsOk());
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 2), false);

  Result<SurfaceTexture> reacquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(reacquired, HasResult());
  EXPECT_THAT(
      SurfaceCalls(*calls),
      ElementsAre("createSurface id=1 canvas=#canvas",
                  "configureSurface surface=1 format=2 usage=1 size=8x8 alphaMode=2",
                  "acquireCurrentTexture surface=1 texture=2", "abandonCurrentTexture surface=1",
                  "configureSurface surface=1 format=2 usage=1 size=16x16 alphaMode=2",
                  "acquireCurrentTexture surface=1 texture=3"));
}

TEST(BrowserDevice, DestroyingASurfaceLetsGoOfTheCanvasContextBehindIt) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(fixture.bridge->objectCount(), 1u);

  ASSERT_THAT(fixture.device->destroySurface(std::move(surface).result()), IsOk());

  // The browser side holds a configured canvas context for as long as this device names it, so a
  // destroyed surface has to say so: leaving it named keeps the canvas configured against a
  // device the caller has finished with.
  EXPECT_THAT(SurfaceCalls(*calls),
              ElementsAre("createSurface id=1 canvas=#canvas", "destroyObject kind=surface id=1"));
  EXPECT_THAT(fixture.bridge->objectCount(), 0u);
  EXPECT_THAT(fixture.device->liveObjectCountForTest(), 0u);
}

TEST(BrowserDevice, DestroyingASurfaceHandsBackItsFrameBeforeLettingGoOfTheCanvas) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  ASSERT_THAT(fixture.device->destroySurface(std::move(surface).result()), IsOk());

  // Order is the contract: the frame belongs to the canvas, so it goes back while the canvas is
  // still named. Releasing the surface first would leave the browser holding a frame for a
  // context nothing can name any more.
  EXPECT_THAT(SurfaceCalls(*calls),
              ElementsAre("createSurface id=1 canvas=#canvas",
                          "configureSurface surface=1 format=2 usage=1 size=8x8 alphaMode=2",
                          "acquireCurrentTexture surface=1 texture=2",
                          "abandonCurrentTexture surface=1", "destroyObject kind=surface id=1"));
  EXPECT_THAT(fixture.bridge->objectCount(), 0u);
  EXPECT_THAT(fixture.device->liveObjectCountForTest(), 0u);
}

TEST(BrowserDevice, DoesNotHandBackAFrameFromAThreadThatDoesNotOwnTheDevice) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  const std::vector<std::string> callsBefore = *fixture.bridge->calls;

  // Dropping the surface hands its frame back, and that names the canvas to the browser, which
  // only the context holding the device may do. Declaration order is the test: the surface is
  // destroyed first, so it is still holding the frame when it goes.
  std::thread other([&] {
    SurfaceTexture frame = std::move(acquired).result();
    Surface dropped = std::move(surface).result();
  });
  other.join();

  // Nothing else ran while the other thread held the handles, so any line added here is a call
  // this device made to the browser from a thread that does not own it.
  EXPECT_THAT(*fixture.bridge->calls, testing::ElementsAreArray(callsBefore));
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Surface, 1), true);
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 2), true);

  // One refusal per release the drop issued, not one per handle: the frame and the canvas
  // context are both released, and both are kept for the owning thread.
  EXPECT_THAT(fixture.device->foreignThreadReleasesForTest(), 2u);
}

TEST(BrowserDevice, DoesNotHandBackAFrameAbandonedFromAThreadThatDoesNotOwnTheDevice) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  const std::vector<std::string> callsBefore = *fixture.bridge->calls;

  // The same refusal covers the explicit request, not only the one a dropped handle issues.
  Status abandoned = Status(GpuError{GpuErrorType::InvalidState, "unset"});
  std::thread other([&] { abandoned = fixture.device->abandonCurrentTexture(surface.result()); });
  other.join();

  // The runtime lets go of its own record either way, so the status is the same; what the
  // refusal withholds is the call to the browser, which is why the frame is still the canvas's
  // until the owning thread hands it back.
  EXPECT_THAT(abandoned, IsOk());
  EXPECT_THAT(*fixture.bridge->calls, testing::ElementsAreArray(callsBefore));
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 2), true);
  EXPECT_THAT(fixture.device->foreignThreadReleasesForTest(), 1u);
}

TEST(BrowserDevice, HandsBackAFrameRefusedElsewhereWhenTheOwningThreadTearsTheDeviceDown) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;
  std::shared_ptr<std::map<BrowserObjectId, BrowserObjectKind>> objects = fixture.bridge->objects;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  const std::vector<std::string> callsBefore = *calls;
  std::thread other([&] {
    SurfaceTexture frame = std::move(acquired).result();
    Surface dropped = std::move(surface).result();
  });
  other.join();
  ASSERT_THAT(*calls, testing::ElementsAreArray(callsBefore));

  // A refused release is deferred, not discarded: teardown runs on the owning thread, so the
  // frame goes back to the canvas there rather than staying with the browser for the life of the
  // page.
  fixture.device.reset();
  EXPECT_THAT(SurfaceCalls(*calls),
              ElementsAre("createSurface id=1 canvas=#canvas",
                          "configureSurface surface=1 format=2 usage=1 size=8x8 alphaMode=2",
                          "acquireCurrentTexture surface=1 texture=2",
                          "abandonCurrentTexture surface=1", "destroyObject kind=surface id=1"));
  EXPECT_THAT(*objects, testing::IsEmpty());
}

TEST(BrowserDevice, HandsBackAFrameRefusedElsewhereBeforeTheOwningThreadTakesTheNextOne) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  // Held across the refusal so the runtime has a second retired texture slot to hand out below;
  // the next frame must not land on the slot the refused one still names, or the hand-back could
  // come from the slot-reuse path rather than from the deferral under test. Which slot comes back
  // depends on the runtime recycling retired slots most-recently-retired first.
  Result<Texture> scratch = fixture.device->createTexture(SimpleTexture(TextureUsage::Sampled));
  ASSERT_THAT(scratch, HasResult());

  Status abandoned = Status(GpuError{GpuErrorType::InvalidState, "unset"});
  const std::vector<std::string> callsBefore = *calls;
  std::thread other([&] {
    SurfaceTexture frame = std::move(acquired).result();
    abandoned = fixture.device->abandonCurrentTexture(surface.result());
  });
  other.join();
  ASSERT_THAT(abandoned, IsOk());
  ASSERT_THAT(*calls, testing::ElementsAreArray(callsBefore));
  ASSERT_THAT(fixture.device->destroyTexture(std::move(scratch).result()), IsOk());

  // The surface outlives the refusal here, so the deferred hand-back happens at the next thing
  // the owning thread asks of it. A canvas refuses a second frame while the first is still
  // named, so without it this acquisition fails rather than merely losing track of a texture.
  Result<SurfaceTexture> reacquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(reacquired, HasResult());
  EXPECT_THAT(
      SurfaceCalls(*calls),
      ElementsAre("createSurface id=1 canvas=#canvas",
                  "configureSurface surface=1 format=2 usage=1 size=8x8 alphaMode=2",
                  "acquireCurrentTexture surface=1 texture=2", "abandonCurrentTexture surface=1",
                  "acquireCurrentTexture surface=1 texture=4"));
}

TEST(BrowserDevice, TearingTheDeviceDownElsewhereStillHandsBackTheFrameItHolds) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  std::shared_ptr<std::vector<std::string>> calls = fixture.bridge->calls;
  std::shared_ptr<std::map<BrowserObjectId, BrowserObjectKind>> objects = fixture.bridge->objects;

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  std::thread other([&] {
    {
      SurfaceTexture frame = std::move(acquired).result();
      Surface dropped = std::move(surface).result();
    }
    fixture.device.reset();
  });
  other.join();

  // Destroying a device off its owning thread is a caller error; what this pins is that refusing
  // a hand-back does not make it worse. Teardown releases everything this device holds whatever
  // thread it runs on, so a frame the refusal above left named would otherwise reach the object
  // sweep and be destroyed, taking away the canvas's own texture rather than handing it back.
  EXPECT_THAT(*calls, Not(Contains(HasSubstr("destroyObject kind=texture"))));
  EXPECT_THAT(SurfaceCalls(*calls),
              ElementsAre("createSurface id=1 canvas=#canvas",
                          "configureSurface surface=1 format=2 usage=1 size=8x8 alphaMode=2",
                          "acquireCurrentTexture surface=1 texture=2",
                          "abandonCurrentTexture surface=1", "destroyObject kind=surface id=1"));
  EXPECT_THAT(*objects, testing::IsEmpty());
}

TEST(BrowserDevice, LosingTheDeviceDuringAFrameStillHandsTheFrameBack) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());
  Result<SurfaceTexture> acquired = fixture.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  fixture.bridge->lost = true;

  // Loss is permanent and refuses everything that draws, but a frame already handed over still
  // has to go back and the canvas context still has to be let go of: refusing releases would
  // strand both for the life of the page.
  ASSERT_THAT(fixture.device->abandonCurrentTexture(surface.result()), IsOk());
  EXPECT_THAT(fixture.bridge->hasObject(BrowserObjectKind::Texture, 2), false);

  ASSERT_THAT(fixture.device->destroySurface(std::move(surface).result()), IsOk());
  EXPECT_THAT(fixture.bridge->objectCount(), 0u);
}

TEST(BrowserDevice, SurfacesABrowserRefusalOfASurfaceIdentifier) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Surface> surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());

  // The browser side checks the identifier and its kind on every surface call, not only on the
  // one that created it, and what it reports is carried through as a handle failure rather than
  // flattened into a generic error.
  fixture.bridge->failOperation = "configureSurface";
  fixture.bridge->failStatus = BridgeStatus::WrongObjectKind;
  EXPECT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("object of another kind")));

  fixture.bridge->failOperation.clear();
  ASSERT_THAT(
      fixture.device->configureSurface(surface.result(), CanvasConfiguration(Extent2d{8, 8})),
      IsOk());

  fixture.bridge->failOperation = "acquireCurrentTexture";
  fixture.bridge->failStatus = BridgeStatus::UnknownObject;
  EXPECT_THAT(fixture.device->acquireCurrentTexture(surface.result()),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                    HasSubstr("no object under this identifier")));
  // A refused acquisition leaves no frame named on either side.
  EXPECT_THAT(fixture.device->liveObjectCountForTest(), 1u);
  EXPECT_THAT(fixture.bridge->objectCount(), 1u);
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

  EXPECT_THAT(fixture.bridge->calls->back(),
              "writeTexture texture=1 bytes=1024 offset=0 bytesPerRow=256 rowsPerImage=4 "
              "destination=(0,0) size=4x4");
}

TEST(BrowserDevice, WritesTexelRowsAtTheRequestedDestination) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> texture = fixture.device->createTexture(SimpleTexture(TextureUsage::CopyDst));
  ASSERT_THAT(texture, HasResult());

  const std::vector<uint8_t> payload(256 * 3, 0u);
  const TexelCopyBufferLayout layout{256, 256, 2};
  ASSERT_THAT(fixture.device->writeTexture(texture.result(), payload, layout, Extent2d{2, 2},
                                           Origin2d{1, 2}),
              IsOk());
  EXPECT_THAT(fixture.bridge->calls->back(),
              "writeTexture texture=1 bytes=768 offset=256 bytesPerRow=256 rowsPerImage=2 "
              "destination=(1,2) size=2x2");
}

TEST(BrowserDevice, RejectsOutOfBoundsTextureWriteBeforeTheBridge) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());

  Result<Texture> texture = fixture.device->createTexture(SimpleTexture(TextureUsage::CopyDst));
  ASSERT_THAT(texture, HasResult());

  const std::vector<uint8_t> payload(256 * 2, 0u);
  const TexelCopyBufferLayout layout{0, 256, 2};
  const std::vector<std::string> callsBefore = *fixture.bridge->calls;
  for (const Origin2d origin : {Origin2d{3, 0}, Origin2d{0, 3}}) {
    EXPECT_THAT(
        fixture.device->writeTexture(texture.result(), payload, layout, Extent2d{2, 2}, origin),
        IsGpuError(GpuErrorType::OutOfBounds));
    EXPECT_THAT(*fixture.bridge->calls, testing::ElementsAreArray(callsBefore));
  }
}

}  // namespace donner::gpu::browser
