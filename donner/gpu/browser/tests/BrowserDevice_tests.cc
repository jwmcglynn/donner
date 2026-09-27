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
/// @param gpuDevice Browser device the bridge runs on; a new one of its own when null, or one
///   another fixture's bridge already runs on, as a further logical device of the same worker.
BrowserFixture MakeDevice(std::shared_ptr<FakeBrowserGpuDevice> gpuDevice = nullptr) {
  auto bridge = gpuDevice != nullptr ? std::make_unique<FakeBrowserBridge>(std::move(gpuDevice))
                                     : std::make_unique<FakeBrowserBridge>();
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

/// A refused bridge allocation must preserve existing objects and permit a clean retry.
template <typename Factory>
void ExpectBridgeCreateFailureAndRetry(BrowserFixture& fixture, std::string_view operation,
                                       Factory&& create) {
  SCOPED_TRACE(operation);
  const auto existing = *fixture.bridge->objects;
  fixture.bridge->failOperation = std::string(operation);
  fixture.bridge->failStatus = BridgeStatus::Failed;
  auto refused = create();
  EXPECT_THAT(refused,
              IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                    AllOf(HasSubstr(operation), HasSubstr("browser refused"))));
  EXPECT_THAT(*fixture.bridge->objects, testing::ContainerEq(existing));
  fixture.bridge->failOperation.clear();
  const auto callsBeforeRefusal = *fixture.bridge->calls;
  fixture.bridge->owned = false;
  auto wrongWorker = create();
  EXPECT_THAT(wrongWorker,
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("another worker")));
  EXPECT_THAT(*fixture.bridge->calls, testing::ContainerEq(callsBeforeRefusal));
  EXPECT_THAT(*fixture.bridge->objects, testing::ContainerEq(existing));
  fixture.bridge->owned = true;
  {
    auto retried = create();
    ASSERT_THAT(retried, HasResult());
    EXPECT_THAT(fixture.bridge->objects->size(), testing::Eq(existing.size() + 1));
  }
  EXPECT_THAT(*fixture.bridge->objects, testing::ContainerEq(existing));
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

TEST(BrowserDeviceRequest, MovingARequestTransfersTheOnlyRightToTakeItsDevice) {
  BrowserDeviceRequest original =
      BrowserDeviceRequest::Begin(std::make_unique<FakeBrowserBridge>());
  BrowserDeviceRequest moved(std::move(original));
  EXPECT_THAT(original.state(), BrowserDeviceRequestState::Failed);
  EXPECT_THAT(original.error().str(), testing::IsEmpty());
  EXPECT_THAT(std::move(original).take(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("consumed")));

  BrowserDeviceRequest assigned = BrowserDeviceRequest::Begin(nullptr);
  assigned = std::move(moved);
  EXPECT_THAT(assigned.state(), BrowserDeviceRequestState::Ready);
  EXPECT_THAT(std::move(moved).take(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("consumed")));
  auto device = std::move(assigned).take();
  ASSERT_THAT(device, HasResult());
  EXPECT_THAT(device.result()->createBuffer(SimpleBuffer(BufferUsage::Vertex)), HasResult());
  EXPECT_THAT(std::move(assigned).take(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("consumed")));
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

TEST(BrowserDeviceRequest, SettlingGivesTheBrowserTheThreadUntilTheRequestSettles) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  bridge->requestState = BrowserDeviceRequestState::Pending;
  FakeBrowserBridge* raw = bridge.get();
  // The browser settles the request on its own event loop, which it only gets while the wait
  // hands the thread over; the third time it does, the request is ready.
  raw->onYield = [raw] {
    if (raw->yieldCount == 3) {
      raw->requestState = BrowserDeviceRequestState::Ready;
    }
  };

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.settle(5.0), BrowserDeviceRequestState::Ready);
  EXPECT_THAT(raw->yieldCount, 3u);
  EXPECT_THAT(raw->yieldedSeconds, testing::Le(0.0031))
      << "each slice must be short, so a request that settles early is not waited out";
  EXPECT_THAT(std::move(request).take(), HasResult());
}

TEST(BrowserDeviceRequest, SettlingGivesUpWhenTheBudgetRunsOut) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  bridge->requestState = BrowserDeviceRequestState::Pending;
  FakeBrowserBridge* raw = bridge.get();

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.settle(0.02), BrowserDeviceRequestState::Pending);
  EXPECT_THAT(raw->yieldCount, testing::Gt(0u));
  EXPECT_THAT(std::move(request).take(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("has not settled")));
}

TEST(BrowserDeviceRequest, SettlingARequestThatAlreadySettledHandsNothingOver) {
  auto bridge = std::make_unique<FakeBrowserBridge>();
  bridge->requestState = BrowserDeviceRequestState::Failed;
  FakeBrowserBridge* raw = bridge.get();

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(bridge));
  EXPECT_THAT(request.settle(5.0), BrowserDeviceRequestState::Failed);
  EXPECT_THAT(raw->yieldCount, 0u);
}

TEST(BrowserDevice, DeclaresALossTheBrowserReportsIntoTheConditionItShares) {
  const auto lostState = std::make_shared<DeviceLostState>();
  auto first = std::make_unique<FakeBrowserBridge>();
  FakeBrowserBridge* firstBridge = first.get();
  Result<std::unique_ptr<BrowserDevice>> firstDevice =
      BrowserDeviceRequest::Begin(std::move(first)).take(lostState);
  ASSERT_THAT(firstDevice, HasResult());
  auto second = std::make_unique<FakeBrowserBridge>(firstBridge->gpuDevice);
  Result<std::unique_ptr<BrowserDevice>> secondDevice =
      BrowserDeviceRequest::Begin(std::move(second)).take(lostState);
  ASSERT_THAT(secondDevice, HasResult());
  EXPECT_THAT(secondDevice.result()->isLost(), testing::IsFalse());

  // The browser reports its device lost; the first device to notice declares the condition every
  // device over it shares, so the other stops waiting on a device that will not answer too.
  firstBridge->gpuDevice->lost = true;
  EXPECT_THAT(firstDevice.result()->createBuffer(SimpleBuffer(BufferUsage::Vertex)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("was lost")));
  EXPECT_THAT(lostState->lost.load(), testing::IsTrue());
  EXPECT_THAT(secondDevice.result()->isLost(), testing::IsTrue());
}

TEST(BrowserDevice, ReportsTheTextureLimitTheBrowserReports) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  // A browser device on hardware that allows more than WebGPU's guaranteed minimum, which the
  // renderer can then use for images, filter regions and layers that large.
  fixture.bridge->maxTextureDimension = 16384;
  EXPECT_THAT(fixture.device->maxTextureDimension2D(), 16384u);
}

TEST(BrowserDevice, FallsBackToTheGuaranteedTextureLimitWhenTheBrowserReportsNone) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  fixture.bridge->maxTextureDimension = 0;
  EXPECT_THAT(fixture.device->maxTextureDimension2D(), 8192u);
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

TEST(BrowserDevice, NonOwningContextCannotReadWriteSubmitOrAcquireExistingResources) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  auto buffer =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(buffer, HasResult());
  auto texture = fixture.device->createTexture(
      SimpleTexture(TextureUsage::CopyDst | TextureUsage::RenderAttachment));
  ASSERT_THAT(texture, HasResult());
  auto view = fixture.device->createTextureView(texture.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());
  auto surface = fixture.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  ASSERT_THAT(fixture.device->configureSurface(surface.result(), CanvasConfiguration({4, 4})),
              IsOk());
  auto mapping = fixture.device->mapBufferAsync(buffer.result(), MapMode::Read, 0, 4);
  ASSERT_THAT(mapping, HasResult());
  fixture.bridge->completeMapping(5, {1, 2, 3, 4});
  auto ready = fixture.device->waitForMapping(mapping.result(), MapWaitParams{0.001, 0.05}, {});
  ASSERT_THAT(ready, HasResult());
  ASSERT_THAT(ready.result().outcome, MapWaitOutcome::Ready);
  auto upload = fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst));
  ASSERT_THAT(upload, HasResult());
  CommandBuffer commands = RecordClearPass(*fixture.device, view.result());
  const auto objectsBefore = *fixture.bridge->objects;
  const auto callsBefore = *fixture.bridge->calls;
  const auto refused =
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("another worker"));
  fixture.bridge->owned = false;

  const std::array<uint8_t, 4> bufferBytes{4, 3, 2, 1};
  const std::vector<uint8_t> textureBytes(1024);
  EXPECT_THAT(fixture.device->writeBuffer(upload.result(), 0, bufferBytes), refused);
  EXPECT_THAT(fixture.device->writeTexture(texture.result(), textureBytes,
                                           TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              refused);
  EXPECT_THAT(fixture.device->mappedBytes(mapping.result()), refused);
  EXPECT_THAT(fixture.device->surfaceCapabilities(surface.result()), refused);
  EXPECT_THAT(fixture.device->configureSurface(surface.result(), CanvasConfiguration({8, 8})),
              refused);
  EXPECT_THAT(fixture.device->acquireCurrentTexture(surface.result()), refused);
  EXPECT_THAT(fixture.device->exportTexture(texture.result()), refused);
  EXPECT_THAT(fixture.device->submit(std::move(commands)), refused);
  EXPECT_THAT(*fixture.bridge->calls, testing::ContainerEq(callsBefore));
  EXPECT_THAT(*fixture.bridge->objects, testing::ContainerEq(objectsBefore));

  fixture.bridge->owned = true;
  EXPECT_THAT(fixture.device->mappedBytes(mapping.result()), HasResult());
  EXPECT_THAT(fixture.device->writeBuffer(upload.result(), 0, bufferBytes), IsOk());
  EXPECT_THAT(fixture.device->acquireCurrentTexture(surface.result()), HasResult());
}

TEST(BrowserDevice, UnavailableSurfaceFrameReleasesItsIdentifierBeforeRetry) {
  for (SurfaceStatus status : {SurfaceStatus::Lost, SurfaceStatus::Timeout}) {
    SCOPED_TRACE(status);
    BrowserFixture fixture = MakeDevice();
    ASSERT_THAT(fixture.device, testing::NotNull());
    auto surface = fixture.device->createSurface(CanvasSurface());
    ASSERT_THAT(surface, HasResult());
    ASSERT_THAT(fixture.device->configureSurface(surface.result(), CanvasConfiguration({4, 4})),
                IsOk());
    fixture.bridge->acquireStatus = status;
    auto unavailable = fixture.device->acquireCurrentTexture(surface.result());
    ASSERT_THAT(unavailable, HasResult());
    EXPECT_THAT(unavailable.result().status, status);
    EXPECT_THAT(unavailable.result().texture.isValid(), testing::IsFalse());
    EXPECT_THAT(fixture.device->liveObjectCountForTest(), 1u);
    EXPECT_THAT(fixture.bridge->objectCount(), 1u);

    fixture.bridge->acquireStatus = SurfaceStatus::Success;
    auto retried = fixture.device->acquireCurrentTexture(surface.result());
    ASSERT_THAT(retried, HasResult());
    EXPECT_THAT(retried.result().status, SurfaceStatus::Success);
    EXPECT_THAT(retried.result().texture.isValid(), testing::IsTrue());
    EXPECT_THAT(fixture.bridge->objectCount(), 2u);
    EXPECT_THAT(fixture.device->abandonCurrentTexture(surface.result()), IsOk());
    EXPECT_THAT(fixture.bridge->objectCount(), 1u);
  }
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

TEST(BrowserDevice, FailedBridgeCreatesPreserveLiveObjectsAndPermitRetry) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  auto texture = fixture.device->createTexture(SimpleTexture(TextureUsage::Sampled));
  ASSERT_THAT(texture, HasResult());
  auto view = fixture.device->createTextureView(texture.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());
  auto sampler = fixture.device->createSampler(SamplerDescriptor{});
  ASSERT_THAT(sampler, HasResult());
  BindGroupLayoutDescriptor layoutDescriptor;
  layoutDescriptor.entries = {
      {0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat, TextureFormat::RGBA8Unorm},
      {1, ShaderStage::Fragment, BindingType::FilteringSampler, TextureFormat::RGBA8Unorm}};
  auto groupLayout = fixture.device->createBindGroupLayout(layoutDescriptor);
  ASSERT_THAT(groupLayout, HasResult());
  BindGroupDescriptor groupDescriptor;
  groupDescriptor.layout = BindGroupLayoutRef(groupLayout.result());
  groupDescriptor.entries = {{0, TextureViewBinding{TextureViewRef(view.result())}},
                             {1, SamplerBinding{SamplerRef(sampler.result())}}};
  PipelineLayoutDescriptor pipelineLayoutDescriptor;
  pipelineLayoutDescriptor.bindGroupLayouts = {BindGroupLayoutRef(groupLayout.result())};
  auto pipelineLayout = fixture.device->createPipelineLayout(PipelineLayoutDescriptor{});
  ASSERT_THAT(pipelineLayout, HasResult());
  auto vertex = fixture.device->createShaderModule(SimpleShaderModule("vertex"));
  ASSERT_THAT(vertex, HasResult());
  auto fragment = fixture.device->createShaderModule(SimpleShaderModule("fragment"));
  ASSERT_THAT(fragment, HasResult());
  RenderPipelineDescriptor pipelineDescriptor;
  pipelineDescriptor.layout = PipelineLayoutRef(pipelineLayout.result());
  pipelineDescriptor.vertex.module = ShaderModuleRef(vertex.result());
  pipelineDescriptor.vertex.entryPoint = RcString("vertexMain");
  pipelineDescriptor.fragment.module = ShaderModuleRef(fragment.result());
  pipelineDescriptor.fragment.entryPoint = RcString("fragmentMain");
  pipelineDescriptor.fragment.targets.push_back(ColorTargetState{});

  ExpectBridgeCreateFailureAndRetry(fixture, "createBuffer", [&] {
    return fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst));
  });
  ExpectBridgeCreateFailureAndRetry(fixture, "createTexture", [&] {
    return fixture.device->createTexture(SimpleTexture(TextureUsage::Sampled));
  });
  ExpectBridgeCreateFailureAndRetry(fixture, "createTextureView", [&] {
    return fixture.device->createTextureView(texture.result(), TextureViewDescriptor{});
  });
  ExpectBridgeCreateFailureAndRetry(
      fixture, "createSampler", [&] { return fixture.device->createSampler(SamplerDescriptor{}); });
  ExpectBridgeCreateFailureAndRetry(fixture, "createBindGroupLayout", [&] {
    return fixture.device->createBindGroupLayout(layoutDescriptor);
  });
  ExpectBridgeCreateFailureAndRetry(
      fixture, "createBindGroup", [&] { return fixture.device->createBindGroup(groupDescriptor); });
  ExpectBridgeCreateFailureAndRetry(fixture, "createPipelineLayout", [&] {
    return fixture.device->createPipelineLayout(pipelineLayoutDescriptor);
  });
  ExpectBridgeCreateFailureAndRetry(fixture, "createShaderModule", [&] {
    return fixture.device->createShaderModule(SimpleShaderModule("retry"));
  });
  ExpectBridgeCreateFailureAndRetry(fixture, "createRenderPipeline", [&] {
    return fixture.device->createRenderPipeline(pipelineDescriptor);
  });

  ShaderModuleDescriptor computeSource = SimpleShaderModule("compute");
  computeSource.computeEntryPoints.push_back(
      ComputeEntryPointInfo{RcString("main"), WorkgroupSize{1, 1, 1}});
  auto computeModule = fixture.device->createShaderModule(computeSource);
  ASSERT_THAT(computeModule, HasResult());
  ComputePipelineDescriptor computeDescriptor;
  computeDescriptor.layout = PipelineLayoutRef(pipelineLayout.result());
  computeDescriptor.compute.module = ShaderModuleRef(computeModule.result());
  computeDescriptor.compute.entryPoint = RcString("main");
  computeDescriptor.workgroupSize = WorkgroupSize{1, 1, 1};
  ExpectBridgeCreateFailureAndRetry(fixture, "createComputePipeline", [&] {
    return fixture.device->createComputePipeline(computeDescriptor);
  });
  ExpectBridgeCreateFailureAndRetry(fixture, "createSurface",
                                    [&] { return fixture.device->createSurface(CanvasSurface()); });
  auto readback =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead));
  ASSERT_THAT(readback, HasResult());
  ExpectBridgeCreateFailureAndRetry(fixture, "mapBufferAsync", [&] {
    return fixture.device->mapBufferAsync(readback.result(), MapMode::Read, 0, 4);
  });
}

TEST(BrowserDevice, PreservesSamplerSettingsAndTextureBindingIdentities) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  fixture.bridge->calls->clear();

  SamplerDescriptor samplerDescriptor{RcString("sampler"), FilterMode::Linear, FilterMode::Nearest,
                                      AddressMode::Repeat, AddressMode::ClampToEdge};
  auto sampler = fixture.device->createSampler(samplerDescriptor);
  ASSERT_THAT(sampler, HasResult());
  auto texture = fixture.device->createTexture(SimpleTexture(TextureUsage::Sampled));
  ASSERT_THAT(texture, HasResult());
  auto view = fixture.device->createTextureView(texture.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());

  BindGroupLayoutDescriptor layoutDescriptor;
  layoutDescriptor.entries = {
      {3, ShaderStage::Fragment, BindingType::SampledTexture2dFloat, TextureFormat::RGBA8Unorm},
      {7, ShaderStage::Fragment, BindingType::FilteringSampler, TextureFormat::RGBA8Unorm}};
  auto layout = fixture.device->createBindGroupLayout(layoutDescriptor);
  ASSERT_THAT(layout, HasResult());
  BindGroupDescriptor groupDescriptor;
  groupDescriptor.layout = BindGroupLayoutRef(layout.result());
  groupDescriptor.entries = {{3, TextureViewBinding{TextureViewRef(view.result())}},
                             {7, SamplerBinding{SamplerRef(sampler.result())}}};
  auto group = fixture.device->createBindGroup(groupDescriptor);
  ASSERT_THAT(group, HasResult());

  EXPECT_THAT(
      *fixture.bridge->calls,
      ElementsAre("createSampler id=1 mag=2 min=1 addressU=2 addressV=1",
                  testing::StartsWith("createTexture id=2 "), "createTextureView id=3 texture=2",
                  testing::StartsWith("createBindGroupLayout id=4 "),
                  "createBindGroup id=5 layout=4 entries=["
                  "(binding=3 resource=3 offset=0 size=0)"
                  "(binding=7 resource=1 offset=0 size=0)]"));
}

TEST(BrowserDevice, PreservesVertexBlendAndIndexedDrawParameters) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  auto target = fixture.device->createTexture(SimpleTexture(TextureUsage::RenderAttachment));
  ASSERT_THAT(target, HasResult());
  auto view = fixture.device->createTextureView(target.result(), TextureViewDescriptor{});
  ASSERT_THAT(view, HasResult());
  auto layout = fixture.device->createPipelineLayout(PipelineLayoutDescriptor{});
  ASSERT_THAT(layout, HasResult());
  auto vertex = fixture.device->createShaderModule(SimpleShaderModule("vertex"));
  ASSERT_THAT(vertex, HasResult());
  auto fragment = fixture.device->createShaderModule(SimpleShaderModule("fragment"));
  ASSERT_THAT(fragment, HasResult());

  RenderPipelineDescriptor descriptor;
  descriptor.layout = PipelineLayoutRef(layout.result());
  descriptor.vertex.module = ShaderModuleRef(vertex.result());
  descriptor.vertex.entryPoint = RcString("vertexMain");
  descriptor.vertex.buffers = {
      {12, VertexStepMode::Vertex, {{VertexFormat::Float32x2, 0, 0}, {VertexFormat::Uint32, 8, 1}}},
      {16, VertexStepMode::Instance, {{VertexFormat::Float32x4, 0, 2}}}};
  descriptor.fragment.module = ShaderModuleRef(fragment.result());
  descriptor.fragment.entryPoint = RcString("fragmentMain");
  descriptor.fragment.targets = {
      {TextureFormat::RGBA8Unorm,
       BlendState{{BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add},
                  {BlendFactor::One, BlendFactor::One, BlendOperation::Max}},
       ColorWriteMask::Red | ColorWriteMask::Alpha}};
  auto pipeline = fixture.device->createRenderPipeline(descriptor);
  ASSERT_THAT(pipeline, HasResult());
  ASSERT_THAT(fixture.bridge->lastRenderPipeline, testing::Optional(testing::_));
  const auto& translated = *fixture.bridge->lastRenderPipeline;
  EXPECT_THAT(
      translated.vertexBuffers,
      ElementsAre(
          testing::FieldsAre(
              12u, 1u, ElementsAre(testing::FieldsAre(1u, 0u, 0u), testing::FieldsAre(3u, 8u, 1u))),
          testing::FieldsAre(16u, 2u, ElementsAre(testing::FieldsAre(2u, 0u, 2u)))));
  EXPECT_THAT(translated.colorTargets,
              ElementsAre(testing::FieldsAre(1u, true, testing::FieldsAre(3u, 4u, 1u),
                                             testing::FieldsAre(2u, 2u, 2u), 9u)));

  auto vertices =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::Vertex | BufferUsage::CopyDst));
  ASSERT_THAT(vertices, HasResult());
  auto indices =
      fixture.device->createBuffer(SimpleBuffer(BufferUsage::Index | BufferUsage::CopyDst));
  ASSERT_THAT(indices, HasResult());
  const std::array<uint8_t, 48> vertexBytes{};
  const std::array<uint8_t, 12> indexBytes{0, 0, 1, 0, 2, 0, 0, 0, 2, 0, 1, 0};
  ASSERT_THAT(fixture.device->writeBuffer(vertices.result(), 16, vertexBytes), IsOk());
  ASSERT_THAT(fixture.device->writeBuffer(indices.result(), 4, indexBytes), IsOk());
  EXPECT_THAT(*fixture.bridge->calls, Contains("writeBuffer buffer=7 offset=16 bytes=48"));
  EXPECT_THAT(*fixture.bridge->calls, Contains("writeBuffer buffer=8 offset=4 bytes=12"));

  auto encoder = fixture.device->createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  RenderPassDescriptor passDescriptor;
  passDescriptor.colorAttachments.push_back(RenderPassColorAttachment{
      TextureViewRef(view.result()), LoadOp::Clear, StoreOp::Store, {0.0, 0.0, 0.0, 1.0}});
  auto pass = encoder.result()->beginRenderPass(passDescriptor);
  ASSERT_THAT(pass, HasResult());
  ASSERT_THAT(pass.result()->setPipeline(pipeline.result()), IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(0, vertices.result(), 16), IsOk());
  ASSERT_THAT(pass.result()->setVertexBuffer(1, vertices.result(), 64), IsOk());
  ASSERT_THAT(pass.result()->setScissorRect(1, 1, 2, 2), IsOk());
  ASSERT_THAT(pass.result()->setViewport(0.5f, 1.0f, 3.0f, 2.0f, 0.25f, 0.75f), IsOk());
  ASSERT_THAT(pass.result()->setIndexBuffer(indices.result(), IndexFormat::Uint16, 4), IsOk());
  ASSERT_THAT(pass.result()->drawIndexed(6, 2), IsOk());
  ASSERT_THAT(pass.result()->end(), IsOk());
  auto commands = encoder.result()->finish();
  ASSERT_THAT(commands, HasResult());
  fixture.bridge->calls->clear();
  ASSERT_THAT(fixture.device->submit(std::move(commands).result()), HasResult());
  EXPECT_THAT(
      *fixture.bridge->calls,
      ElementsAre(
          "beginCommandBuffer serial=1 index=0",
          "beginRenderPass attachments=[(view=2 load=1 store=1 "
          "clear=[0.000,0.000,0.000,1.000])]",
          "setRenderPipeline pipeline=6", "setVertexBuffer slot=0 buffer=7 offset=16",
          "setVertexBuffer slot=1 buffer=7 offset=64", "setScissorRect x=1 y=1 size=2x2",
          "setViewport x=0.500 y=1.000 size=3.000x2.000 depth=0.250..0.750",
          "setIndexBuffer buffer=8 format=1 offset=4",
          "drawIndexed indexCount=6 instanceCount=2 firstIndex=0 baseVertex=0 firstInstance=0",
          "endRenderPass", "endCommandBuffer serial=1", "submitCommandBuffers serial=1 count=1"));
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

// Texture sharing between runtime devices over one browser device. A worker drawing through the
// browser backend reads every tile back through a capture context: a second runtime device over
// the same browser device, on the same thread, that registers the texture the renderer drew. The
// fixtures below stand two devices over one fake browser device for that, and one over another for
// the refusals.

namespace {

/// Extent of the texture \ref ShareableTexture describes.
constexpr Extent2d kShareableExtent{4, 4};

/// A texture that can be sampled and copied from, as a snapshot target is.
TextureDescriptor ShareableTexture() {
  return TextureDescriptor{
      RcString("snapshot"), kShareableExtent, TextureFormat::RGBA8Unorm,
      TextureUsage::RenderAttachment | TextureUsage::Sampled | TextureUsage::CopySrc, 1};
}

}  // namespace

TEST(BrowserDeviceSharing, ExportsATextureForAnotherDeviceOverTheSameBrowserDevice) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  Result<Texture> texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());

  Result<TextureExport> exported = producer.device->exportTexture(texture.result());
  ASSERT_THAT(exported, HasResult());
  EXPECT_THAT(exported.result().descriptor().size, kShareableExtent);
  EXPECT_THAT(producer.bridge->gpuDevice->isTextureLive(*producer.bridge->nativeTextureOf(1)),
              testing::IsTrue());
}

TEST(BrowserDeviceSharing, FailedExportAndRegistrationLeaveTheTextureAvailableForRetry) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  BrowserFixture consumer = MakeDevice(producer.bridge->gpuDevice);
  ASSERT_THAT(consumer.device, testing::NotNull());
  auto texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  const auto nativeTexture = producer.bridge->nativeTextureOf(1);
  ASSERT_THAT(nativeTexture, testing::Ne(std::nullopt));

  producer.bridge->failOperation = "shareTexture";
  EXPECT_THAT(producer.device->exportTexture(texture.result()),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("exportTexture")));
  EXPECT_THAT(producer.bridge->gpuDevice->liveShares(), 0u);
  EXPECT_THAT(producer.bridge->gpuDevice->isTextureLive(*nativeTexture), testing::IsTrue());
  producer.bridge->failOperation.clear();
  auto exported = producer.device->exportTexture(texture.result());
  ASSERT_THAT(exported, HasResult());

  consumer.bridge->failOperation = "registerSharedTexture";
  EXPECT_THAT(consumer.device->registerTexture(exported.result()),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("registerTexture")));
  EXPECT_THAT(*consumer.bridge->objects, testing::IsEmpty());
  EXPECT_THAT(producer.bridge->gpuDevice->liveShares(), 1u);
  consumer.bridge->failOperation.clear();
  {
    auto registered = consumer.device->registerTexture(exported.result());
    ASSERT_THAT(registered, HasResult());
    EXPECT_THAT(consumer.bridge->nativeTextureOf(2), nativeTexture);
  }
  EXPECT_THAT(*consumer.bridge->objects, testing::IsEmpty());
  EXPECT_THAT(producer.bridge->gpuDevice->isTextureLive(*nativeTexture), testing::IsTrue());
}

TEST(BrowserDeviceSharing, RegistersAnExportAsAReadOnlyAliasOfTheSameBrowserTexture) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  BrowserFixture consumer = MakeDevice(producer.bridge->gpuDevice);
  ASSERT_THAT(consumer.device, testing::NotNull());

  Result<Texture> texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  Result<TextureExport> exported = producer.device->exportTexture(texture.result());
  ASSERT_THAT(exported, HasResult());

  const size_t consumerCallsBefore = consumer.bridge->calls->size();
  Result<Texture> registration = consumer.device->registerTexture(exported.result());
  ASSERT_THAT(registration, HasResult());

  // Each logical device numbers its own identifiers; the registration's names the texture the
  // producer allocated, and allocates nothing of its own.
  EXPECT_THAT(consumer.bridge->nativeTextureOf(1), producer.bridge->nativeTextureOf(1));
  EXPECT_THAT(consumer.device->ownsTextureBacking(registration.result()), testing::IsFalse());
  const std::vector<std::string> registrationCalls(
      consumer.bridge->calls->begin() + static_cast<ptrdiff_t>(consumerCallsBefore),
      consumer.bridge->calls->end());
  EXPECT_THAT(registrationCalls, Not(Contains(testing::StartsWith("createTexture "))));

  // The consumer reads what the producer drew through its own identifier.
  BufferDescriptor readbackDescriptor = SimpleBuffer(BufferUsage::CopyDst | BufferUsage::MapRead);
  readbackDescriptor.byteSize = 1024;
  Result<Buffer> readback = consumer.device->createBuffer(readbackDescriptor);
  ASSERT_THAT(readback, HasResult());
  Result<std::unique_ptr<CommandEncoder>> encoder = consumer.device->createCommandEncoder();
  ASSERT_THAT(encoder, HasResult());
  std::unique_ptr<CommandEncoder> commands = std::move(encoder).result();
  ASSERT_THAT(commands->copyTextureToBuffer(TexelCopyTextureInfo{TextureRef(registration.result())},
                                            readback.result(), TexelCopyBufferLayout{0, 256, 4},
                                            Extent2d{4, 4}),
              IsOk());
  Result<CommandBuffer> commandBuffer = commands->finish();
  ASSERT_THAT(commandBuffer, HasResult());
  EXPECT_THAT(consumer.device->submit(std::move(commandBuffer).result()), HasResult());
  EXPECT_THAT(*consumer.bridge->calls,
              Contains("copyTextureToBuffer texture=1 buffer=2 offset=0 bytesPerRow=256 "
                       "rowsPerImage=4 size=4x4"));
}

TEST(BrowserDeviceSharing, KeepsTheBrowserTextureUntilItsLastHolderLetsGo) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  BrowserFixture consumer = MakeDevice(producer.bridge->gpuDevice);
  ASSERT_THAT(consumer.device, testing::NotNull());
  const std::shared_ptr<FakeBrowserGpuDevice> gpuDevice = producer.bridge->gpuDevice;

  Result<Texture> texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  const uint64_t native = *producer.bridge->nativeTextureOf(1);
  Result<TextureExport> exported = producer.device->exportTexture(texture.result());
  ASSERT_THAT(exported, HasResult());
  Result<Texture> registration = consumer.device->registerTexture(exported.result());
  ASSERT_THAT(registration, HasResult());

  EXPECT_THAT(producer.device->destroyTexture(std::move(texture).result()), IsOk());
  EXPECT_THAT(gpuDevice->isTextureLive(native), testing::IsTrue())
      << "the producer's release destroyed a texture the consumer still reads";

  EXPECT_THAT(consumer.device->destroyTexture(std::move(registration).result()), IsOk());
  EXPECT_THAT(gpuDevice->isTextureLive(native), testing::IsTrue())
      << "releasing a registration destroyed a texture the export token still holds";

  exported = TextureExport();
  EXPECT_THAT(gpuDevice->isTextureLive(native), testing::IsFalse())
      << "the texture outlived its last holder";
}

TEST(BrowserDeviceSharing, AnUnsharedTextureIsStillDestroyedWhenItsDeviceReleasesIt) {
  BrowserFixture fixture = MakeDevice();
  ASSERT_THAT(fixture.device, testing::NotNull());
  Result<Texture> texture = fixture.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  const uint64_t native = *fixture.bridge->nativeTextureOf(1);

  EXPECT_THAT(fixture.device->destroyTexture(std::move(texture).result()), IsOk());
  EXPECT_THAT(fixture.bridge->gpuDevice->isTextureLive(native), testing::IsFalse());
}

TEST(BrowserDeviceSharing, ARegistrationOutlivesItsProducerDevice) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  BrowserFixture consumer = MakeDevice(producer.bridge->gpuDevice);
  ASSERT_THAT(consumer.device, testing::NotNull());
  const std::shared_ptr<FakeBrowserGpuDevice> gpuDevice = producer.bridge->gpuDevice;

  Result<Texture> texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  const uint64_t native = *producer.bridge->nativeTextureOf(1);
  Result<Texture> registration = Texture();
  {
    Result<TextureExport> exported = producer.device->exportTexture(texture.result());
    ASSERT_THAT(exported, HasResult());
    registration = consumer.device->registerTexture(exported.result());
  }
  ASSERT_THAT(registration, HasResult());

  producer.device.reset();
  EXPECT_THAT(gpuDevice->isTextureLive(native), testing::IsTrue())
      << "tearing the producer down destroyed a texture the consumer still reads";
  EXPECT_THAT(consumer.device->createTextureView(registration.result(),
                                                 TextureViewDescriptor{RcString("view")}),
              HasResult());

  consumer.device.reset();
  EXPECT_THAT(gpuDevice->isTextureLive(native), testing::IsFalse());
}

TEST(BrowserDeviceSharing, SharesASurfaceFrameWithoutEverDestroyingTheCanvasTexture) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  BrowserFixture consumer = MakeDevice(producer.bridge->gpuDevice);
  ASSERT_THAT(consumer.device, testing::NotNull());
  const std::shared_ptr<FakeBrowserGpuDevice> gpuDevice = producer.bridge->gpuDevice;

  Result<Surface> surface = producer.device->createSurface(CanvasSurface());
  ASSERT_THAT(surface, HasResult());
  // A canvas configured to be read back, so a registration of its frame has a use.
  SurfaceConfiguration configuration = CanvasConfiguration(Extent2d{8, 8});
  configuration.usage = TextureUsage::RenderAttachment | TextureUsage::CopySrc;
  ASSERT_THAT(producer.device->configureSurface(surface.result(), configuration), IsOk());
  Result<SurfaceTexture> acquired = producer.device->acquireCurrentTexture(surface.result());
  ASSERT_THAT(acquired, HasResult());

  // Every device over the browser device submits to its one queue, so a frame the surface has out
  // is shared like any other texture: a reader's work is ordered before the canvas takes it back.
  Result<TextureExport> exported = producer.device->exportTexture(acquired.result().texture);
  ASSERT_THAT(exported, HasResult());
  Result<Texture> registration = consumer.device->registerTexture(exported.result());
  ASSERT_THAT(registration, HasResult());
  // The surface is identifier 1 on the producer and its frame 2; the registration is the
  // consumer's first identifier.
  const std::optional<uint64_t> frame = producer.bridge->nativeTextureOf(2);
  ASSERT_THAT(frame.has_value(), testing::IsTrue());
  EXPECT_THAT(consumer.bridge->nativeTextureOf(1), frame);

  // The canvas owns the frame, so neither its surface taking it back nor its last holder letting
  // go destroys it.
  EXPECT_THAT(consumer.device->destroyTexture(std::move(registration).result()), IsOk());
  ASSERT_THAT(producer.device->abandonCurrentTexture(surface.result()), IsOk());
  exported = TextureExport();
  EXPECT_THAT(gpuDevice->releasedShares, 1u);
  EXPECT_THAT(gpuDevice->isTextureLive(*frame), testing::IsTrue())
      << "releasing a share destroyed the canvas's own texture";
}

TEST(BrowserDeviceSharing, RefusesATextureOfAnotherBrowserDevice) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  BrowserFixture elsewhere = MakeDevice();
  ASSERT_THAT(elsewhere.device, testing::NotNull());

  Result<Texture> texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  Result<TextureExport> exported = producer.device->exportTexture(texture.result());
  ASSERT_THAT(exported, HasResult());

  // WebGPU cannot share a texture across GPU devices, so a device over another one refuses it
  // with the runtime's own reason.
  EXPECT_THAT(elsewhere.device->registerTexture(exported.result()),
              IsGpuErrorWithMessage(GpuErrorType::DeviceMismatch,
                                    HasSubstr("belongs to a different native device")));
}

TEST(BrowserDeviceSharing, AnExportLetGoOnAnotherThreadIsReleasedWhenItsOwnerNextYields) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  BrowserFixture consumer = MakeDevice(producer.bridge->gpuDevice);
  ASSERT_THAT(consumer.device, testing::NotNull());
  const std::shared_ptr<FakeBrowserGpuDevice> gpuDevice = producer.bridge->gpuDevice;

  Result<Texture> texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  const uint64_t native = *producer.bridge->nativeTextureOf(1);
  Result<TextureExport> exported = producer.device->exportTexture(texture.result());
  ASSERT_THAT(exported, HasResult());
  // The producer device goes, so the export is the texture's last holder.
  EXPECT_THAT(producer.device->destroyTexture(std::move(texture).result()), IsOk());
  producer.device.reset();
  ASSERT_THAT(gpuDevice->isTextureLive(native), testing::IsTrue());

  // A token may be let go on any thread, but only the worker that made the share can reach its
  // browser side, so the release waits for that worker rather than being lost.
  std::thread elsewhere(
      [token = std::move(exported).result()]() mutable { token = TextureExport(); });
  elsewhere.join();
  EXPECT_THAT(gpuDevice->isTextureLive(native), testing::IsTrue())
      << "a release reached the browser device from a thread that does not own it";

  consumer.bridge->yieldToBrowser(0.0);
  EXPECT_THAT(gpuDevice->releasedShares, 1u)
      << "the owner yielded and the share let go on another thread was still not released";
  EXPECT_THAT(gpuDevice->isTextureLive(native), testing::IsFalse())
      << "the texture outlived its last holder for as long as the browser device lives";
}

TEST(BrowserDeviceSharing, AReleaseThatArrivesAfterTheBrowserDeviceIsGoneDoesNothing) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  const std::shared_ptr<FakeBrowserGpuDevice> gpuDevice = producer.bridge->gpuDevice;
  Result<Texture> texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  const uint64_t native = *producer.bridge->nativeTextureOf(1);
  Result<TextureExport> onOwner = producer.device->exportTexture(texture.result());
  ASSERT_THAT(onOwner, HasResult());
  Result<Texture> second = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(second, HasResult());
  Result<TextureExport> elsewhereToken = producer.device->exportTexture(second.result());
  ASSERT_THAT(elsewhereToken, HasResult());

  // The last logical device goes, and the browser device with it, taking every texture its shares
  // held.
  producer.device.reset();
  gpuDevice->release();
  ASSERT_THAT(gpuDevice->isTextureLive(native), testing::IsFalse());

  // Both late releases, on the owner's thread and on another, find nothing and do nothing.
  onOwner = TextureExport();
  std::thread elsewhere(
      [token = std::move(elsewhereToken).result()]() mutable { token = TextureExport(); });
  elsewhere.join();
  MakeDevice(gpuDevice).bridge->yieldToBrowser(0.0);
  EXPECT_THAT(gpuDevice->releasedShares, 0u);
  EXPECT_THAT(gpuDevice->liveShares(), 0u);
}

TEST(BrowserDeviceSharing, RefusesARegistrationFromAThreadThatDoesNotOwnTheDevice) {
  BrowserFixture producer = MakeDevice();
  ASSERT_THAT(producer.device, testing::NotNull());
  BrowserFixture consumer = MakeDevice(producer.bridge->gpuDevice);
  ASSERT_THAT(consumer.device, testing::NotNull());

  Result<Texture> texture = producer.device->createTexture(ShareableTexture());
  ASSERT_THAT(texture, HasResult());
  Result<TextureExport> exported = producer.device->exportTexture(texture.result());
  ASSERT_THAT(exported, HasResult());

  Result<Texture> registration = Texture();
  std::thread elsewhere(
      [&] { registration = consumer.device->registerTexture(exported.result()); });
  elsewhere.join();
  EXPECT_THAT(registration, IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                  HasSubstr("cannot be used from another thread")));
  EXPECT_THAT(consumer.bridge->objectCount(), 0u);
}

}  // namespace donner::gpu::browser
