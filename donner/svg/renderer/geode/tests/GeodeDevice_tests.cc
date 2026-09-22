#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::geode {

using svg::test::RgbaEq;
using testing::Eq;
using testing::HasSubstr;
using testing::IsNull;
using testing::Not;
using testing::NotNull;

/// Marker that `GeodeDevice`'s uncaptured-error callback prints when wgpu
/// rejects a descriptor. wgpu still returns a non-null handle in that case, so
/// tests that need to know a resource was actually accepted watch for this.
constexpr const char* kWgpuUncapturedErrorMarker = "Uncaptured error";

TEST(GeodeCallbackState, CallbackOwnsStateAfterCallerReturns) {
  struct State {};

  auto state = std::make_shared<State>();
  const std::weak_ptr<State> weakState = state;
  void* userdata = retainWgpuCallbackState(state);

  state.reset();
  EXPECT_FALSE(weakState.expired());

  std::shared_ptr<State> callbackState = takeWgpuCallbackState<State>(userdata);
  EXPECT_EQ(callbackState.get(), weakState.lock().get());
  callbackState.reset();
  EXPECT_TRUE(weakState.expired());
}

/// Smoke test: can we instantiate a headless Dawn device at all?
/// If this fails, the entire Geode backend is non-functional.
TEST(GeodeDevice, CreateHeadlessSucceeds) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr) << "Failed to create headless Dawn device. Check driver availability "
                                "(Metal on macOS, Vulkan/SwiftShader on Linux).";

  EXPECT_TRUE(static_cast<bool>(device->device()));
  EXPECT_TRUE(static_cast<bool>(device->queue()));
  EXPECT_TRUE(static_cast<bool>(device->adapter()));
}

TEST(GeodeDevice, DestructionConsumesDeviceLostCallbackState) {
  const std::size_t before = GeodeDevice::outstandingDeviceLostCallbacksForTesting();
  {
    auto device = GeodeDevice::CreateHeadless();
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(GeodeDevice::outstandingDeviceLostCallbacksForTesting(), before + 1u);
  }

  EXPECT_EQ(GeodeDevice::outstandingDeviceLostCallbacksForTesting(), before);
}

namespace {

/// One `GeodeEmbedConfig` field that must agree with the shared physical owner the same config
/// names.
struct EmbedConfigConflictArm {
  /// Field name, so a failure says which check is missing.
  std::string_view field;
  /// Overwrites that field with state that cannot belong to the owner.
  void (*applyConflict)(GeodeEmbedConfig& config, GeodeDevice& foreign);
};

constexpr auto kEmbedConfigConflictArms = std::to_array<EmbedConfigConflictArm>(
    {{"device",
      [](GeodeEmbedConfig& config, GeodeDevice& foreign) { config.device = foreign.device(); }},
     {"queue",
      [](GeodeEmbedConfig& config, GeodeDevice& foreign) { config.queue = foreign.queue(); }},
     {"instance",
      [](GeodeEmbedConfig& config, GeodeDevice& foreign) { config.instance = foreign.instance(); }},
     {"adapter",
      [](GeodeEmbedConfig& config, GeodeDevice& foreign) { config.adapter = foreign.adapter(); }},
     // The owner's own loss state is private to GeodeDevice and EditorWindow, so a second
     // context's flag is not reachable here. The check is shared_ptr identity, so any distinct
     // state is a faithful conflict.
     {"lostState", [](GeodeEmbedConfig& config, GeodeDevice&) {
        config.lostState = std::make_shared<GeodeDeviceLostState>();
      }}});

}  // namespace

/// A config that names a shared physical owner may also repeat that owner's roots, but every
/// repeated field has to name the same object. Mixing in a root that belongs elsewhere is a caller
/// error with no safe resolution - silently preferring either side would hand the context a queue,
/// instance, or loss flag that does not belong to the device it renders on - so creation is
/// refused for each field independently, and accepted when the repeated roots agree.
TEST(GeodeDevice, SharedPhysicalOwnerRejectsConflictingRoots) {
  auto ownerContext = GeodeDevice::CreateHeadless();
  ASSERT_NE(ownerContext, nullptr);
  auto foreignContext = GeodeDevice::CreateHeadless();
  ASSERT_NE(foreignContext, nullptr);
  // Each root the arms below borrow has to be non-null, because the comparison skips a field the
  // config leaves empty. A headless context imported from the browser leaves its adapter on the
  // JavaScript side, and borrowing that null adapter would make the adapter arm assert nothing
  // about the adapter comparison and then fail for a reason that has nothing to do with it. Say so
  // here instead.
  ASSERT_THAT(static_cast<WGPUAdapter>(foreignContext->adapter()), NotNull())
      << "the conflict arms need a second context with every root populated";

  for (const EmbedConfigConflictArm& arm : kEmbedConfigConflictArms) {
    SCOPED_TRACE(arm.field);

    GeodeEmbedConfig config;
    config.physicalDevice = ownerContext->physicalDeviceOwner();
    arm.applyConflict(config, *foreignContext);

    EXPECT_THAT(GeodeDevice::CreateFromExternal(config), IsNull())
        << "CreateFromExternal accepted a config whose " << arm.field
        << " belongs to a different physical device than its physical owner";
  }

  // Positive control: without it, a change that refused every config naming a physical owner
  // would leave all five arms green while the comparisons they cover stopped running.
  GeodeEmbedConfig agreeing;
  agreeing.physicalDevice = ownerContext->physicalDeviceOwner();
  agreeing.device = ownerContext->device();
  agreeing.queue = ownerContext->queue();
  agreeing.instance = ownerContext->instance();
  agreeing.adapter = ownerContext->adapter();
  EXPECT_THAT(GeodeDevice::CreateFromExternal(agreeing), NotNull())
      << "CreateFromExternal refused a config that repeats its own physical owner's roots";
}

TEST(GeodeDevice, LegacyBorrowedAggregateConfigurationRemainsSupported) {
  auto ownerContext = GeodeDevice::CreateHeadless();
  ASSERT_NE(ownerContext, nullptr);

  GeodeEmbedConfig config{ownerContext->instance(), ownerContext->device(),
                          ownerContext->queue(),    wgpu::TextureFormat::RGBA8Unorm,
                          ownerContext->adapter(),  std::make_shared<GeodeDeviceLostState>()};
  EXPECT_NE(GeodeDevice::CreateFromExternal(config), nullptr);
}

TEST(GeodeDevice, SharedPhysicalOwnerRejectsAlreadyLostDevice) {
  auto ownerContext = GeodeDevice::CreateHeadless();
  ASSERT_NE(ownerContext, nullptr);

  auto lostState = std::make_shared<GeodeDeviceLostState>();
  auto borrowedOwner = GeodePhysicalDeviceOwner::CreateBorrowed(
      ownerContext->instance(), ownerContext->adapter(), ownerContext->device(),
      ownerContext->queue(), lostState);
  lostState->lost.store(true, std::memory_order_release);

  GeodeEmbedConfig config;
  config.physicalDevice = std::move(borrowedOwner);
  EXPECT_EQ(GeodeDevice::CreateFromExternal(config), nullptr);
}

/// Can we allocate an offscreen render-target texture?
TEST(GeodeDevice, CanCreateRenderTargetTexture) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  wgpu::TextureDescriptor desc = {};
  desc.label = wgpuLabel("TestRenderTarget");
  desc.size = {64, 64, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  desc.dimension = wgpu::TextureDimension::_2D;

  wgpu::Texture texture = device->device().createTexture(desc);
  ASSERT_TRUE(static_cast<bool>(texture));
  EXPECT_EQ(texture.getWidth(), 64u);
  EXPECT_EQ(texture.getHeight(), 64u);
}

/// Can we allocate a buffer for readback?
TEST(GeodeDevice, CanCreateReadbackBuffer) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  wgpu::BufferDescriptor desc = {};
  desc.label = wgpuLabel("TestReadbackBuffer");
  desc.size = 1024;
  desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;

  wgpu::Buffer buffer = device->device().createBuffer(desc);
  ASSERT_TRUE(static_cast<bool>(buffer));
  EXPECT_EQ(buffer.getSize(), 1024u);
}

/// End-to-end: clear a texture to red and read back the first pixel.
/// This proves that command submission and texture readback actually work.
TEST(GeodeDevice, CanExecuteClearAndReadback) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  const wgpu::Device& device = geodeDevice->device();
  const wgpu::Queue& queue = geodeDevice->queue();

  constexpr uint32_t kSize = 4;  // Small texture for a quick test.

  // Create render target.
  wgpu::TextureDescriptor texDesc = {};
  texDesc.size = {kSize, kSize, 1};
  texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  texDesc.mipLevelCount = 1;
  texDesc.sampleCount = 1;
  texDesc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture target = device.createTexture(texDesc);
  ASSERT_TRUE(static_cast<bool>(target));

  // Create readback buffer. Bytes per row must be a multiple of 256 per WebGPU spec.
  constexpr uint32_t kBytesPerRow = 256;  // Padded from kSize*4=16.
  constexpr uint32_t kBufferSize = kBytesPerRow * kSize;
  wgpu::BufferDescriptor bufDesc = {};
  bufDesc.size = kBufferSize;
  bufDesc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  wgpu::Buffer readback = device.createBuffer(bufDesc);

  // Encode: clear to red, then copy to buffer.
  wgpu::CommandEncoder encoder = device.createCommandEncoder();

  wgpu::RenderPassColorAttachment colorAttachment = {};
  colorAttachment.view = target.createView();
  colorAttachment.loadOp = wgpu::LoadOp::Clear;
  colorAttachment.storeOp = wgpu::StoreOp::Store;
  colorAttachment.clearValue = {1.0, 0.0, 0.0, 1.0};        // Red.
  colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;  // Dawn requires this on 2D views.

  wgpu::RenderPassDescriptor passDesc = {};
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &colorAttachment;

  wgpu::RenderPassEncoder pass = encoder.beginRenderPass(passDesc);
  pass.end();

  wgpu::TexelCopyTextureInfo src = {};
  src.texture = target;
  src.mipLevel = 0;
  src.origin = {0, 0, 0};

  wgpu::TexelCopyBufferInfo dst = {};
  dst.buffer = readback;
  dst.layout.bytesPerRow = kBytesPerRow;
  dst.layout.rowsPerImage = kSize;

  wgpu::Extent3D copySize = {kSize, kSize, 1};
  encoder.copyTextureToBuffer(src, dst, copySize);

  wgpu::CommandBuffer commands = encoder.finish();
  queue.submit(1, &commands);

  // Map the buffer synchronously. wgpu-native's `mapAsync` only accepts a
  // `BufferMapCallbackInfo` with a raw C callback + void* userdata, so we
  // hand the done flag through userdata1 and spin on `device.poll(true)`
  // until wgpu-native drains the pending callback.
  struct MapState {
    std::atomic<bool> done = false;
    std::atomic<bool> ok = false;
  };
  auto mapState = std::make_shared<MapState>();
  wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
  mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1,
                      void* /*userdata2*/) {
    const std::shared_ptr<MapState> state = takeWgpuCallbackState<MapState>(userdata1);
    state->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
    state->done.store(true, std::memory_order_release);
    if (!state->ok.load(std::memory_order_relaxed)) {
      (void)message;  // Keep the message parameter named for future logging.
    }
  };
  mapCb.userdata1 = retainWgpuCallbackState(mapState);
  mapCb.userdata2 = nullptr;
  readback.mapAsync(wgpu::MapMode::Read, 0, kBufferSize, mapCb);

  const GpuWaitResult waitResult = BoundedGpuWait(
      [&] {
        device.poll(false, nullptr);
        return mapState->done.load(std::memory_order_acquire);
      },
      kDefaultGpuWaitTimeout);
  ASSERT_EQ(waitResult, GpuWaitResult::Complete) << "buffer map wait timed out";
  EXPECT_TRUE(mapState->ok.load(std::memory_order_relaxed)) << "buffer map failed";

  const uint8_t* pixels = static_cast<const uint8_t*>(readback.getConstMappedRange(0, kBufferSize));
  ASSERT_NE(pixels, nullptr);

  // First pixel should be red (255, 0, 0, 255).
  const std::array<uint8_t, 4> firstPixel = {pixels[0], pixels[1], pixels[2], pixels[3]};
  EXPECT_THAT(firstPixel, RgbaEq(255, 0, 0, 255));

  readback.unmap();
}

/// `runtimeDevice()` and `adapterDevice()` are two names for one object: the second only widens
/// the static type for the callers that still need operations the runtime contract does not carry.
/// If they ever came apart, renderer services would create resources in one handle table and the
/// remaining raw callers would validate them against another, so a handle would go foreign for no
/// visible reason. Pin the identity while both accessors exist.
TEST(GeodeDevice, RuntimeAndAdapterAccessorsNameOneDevice) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  EXPECT_THAT(device->runtimeDevice().deviceId(), Eq(device->adapterDevice().deviceId()));
  EXPECT_EQ(&device->runtimeDevice(), static_cast<gpu::Device*>(&device->adapterDevice()))
      << "the two accessors named different objects, so renderer services would create resources "
         "in one handle table while the remaining raw callers validate them against another";
}

/// Regression test for issue #575 (pipeline leak through wgpu-native):
/// `GeodeDevice::pipeline()` / `gradientPipeline()` / `imagePipeline()` /
/// `filterEngine()` must return the same object on every call - the
/// expensive wgpu pipelines live on the device, not on per-renderer
/// state. If someone moves pipeline construction back into
/// `RendererGeode::Impl::initPipelines`, the ~1.6 MB/renderer leak that
/// exhausted Mesa lavapipe's allocation budget comes back. Asserting
/// reference identity is a cheap way to pin the sharing contract.
TEST(GeodeDevice, SharedPipelinesReturnSameInstance) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  // Every accessor must return the same reference each time.
  EXPECT_EQ(&device->pipeline(), &device->pipeline());
  EXPECT_EQ(&device->gradientPipeline(), &device->gradientPipeline());
  EXPECT_EQ(&device->imagePipeline(), &device->imagePipeline());
  EXPECT_EQ(&device->filterEngine(), &device->filterEngine());
  // `maskPipeline()` is lazy - two calls must still return the same
  // instance (first call constructs, second call returns cached).
  EXPECT_EQ(&device->maskPipeline(), &device->maskPipeline());
}

/// The record-reading variant of the fill pipeline is compiled lazily, and
/// only a cross-entity batch ever asks for it. A build that leaves scene
/// batching switched off therefore never constructs it, so a renamed shader
/// entry point, or a bind group layout the batched entry points no longer
/// match, would pass every test and only fail once batching is enabled.
/// Compile it here on the real device, unconditionally and independent of the
/// batching switch, so both variants stay covered.
///
/// The returned handle alone proves nothing: the backend hands back a non-null
/// error pipeline for a rejected descriptor and reports the reason through the
/// device's uncaptured-error callback, so the real assertion is that the
/// callback printed nothing. The shader-emitter validation tests keep a
/// negative control proving this marker does fire on a bad descriptor.
TEST(GeodeDevice, BatchedFillPipelineCompilesWithoutValidationErrors) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  testing::internal::CaptureStderr();
  const gpu::RenderPipeline& batched = device->pipeline().batchedPipeline();
  const std::string errors = testing::internal::GetCapturedStderr();

  EXPECT_TRUE(batched.isValid()) << "Batched fill pipeline creation returned null.";
  EXPECT_THAT(errors, Not(HasSubstr(kWgpuUncapturedErrorMarker)))
      << "The backend rejected the record-reading fill pipeline:\n"
      << errors;

  // Built on first use, then cached: a second call must hand back the same
  // pipeline rather than recompiling it.
  EXPECT_EQ(&device->pipeline().batchedPipeline(), &batched);
}

/// Regression test for issue #575: texture / buffer allocation
/// must not grow unboundedly under a busy-idle pattern that hits the
/// device's shared pipelines. Ten `countTexture` / `countBuffer`
/// ticks with no actual wgpu work between them must show exactly the
/// reported growth in `lifetimeTextureCreates()` / `lifetimeBufferCreates()`
/// - this locks the accessor contract so a leak-hunt regression
/// test written against it can't lie to itself.
TEST(GeodeDevice, LifetimeCountersReflectCountHelpers) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  const uint64_t beforeTex = device->lifetimeTextureCreates();
  const uint64_t beforeBuf = device->lifetimeBufferCreates();
  for (int i = 0; i < 10; ++i) {
    device->countTexture();
    device->countBuffer();
  }
  EXPECT_EQ(device->lifetimeTextureCreates(), beforeTex + 10);
  EXPECT_EQ(device->lifetimeBufferCreates(), beforeBuf + 10);
}

/// The device-lost flag is observable, sticky, and logged once.
TEST(GeodeDeviceLost, MarkDeviceLostIsSticky) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  EXPECT_FALSE(device->isDeviceLost());
  device->markDeviceLost("test-injected loss");
  EXPECT_TRUE(device->isDeviceLost());
  device->markDeviceLost("second call must be a no-op");
  EXPECT_TRUE(device->isDeviceLost());
}

/// A timeout-declared loss carries the attribution that makes it diagnosable:
/// which bounded wait expired and how long it actually ran.
TEST(GeodeDeviceLost, WaitTimeoutRecordsSiteAndElapsed) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  const GeodeDevice::ReadbackStats healthy = device->consumeReadbackStats();
  EXPECT_FALSE(healthy.deviceLost);
  EXPECT_EQ(healthy.timedOutWaitSite, GpuWaitSite::None);
  EXPECT_EQ(healthy.timedOutWaitMs, 0);

  device->markDeviceLostAfterWaitTimeout(GpuWaitSite::ReadbackMap, kReadbackMapTimeout,
                                         "test-injected readback map stall");

  EXPECT_TRUE(device->isDeviceLost());
  const GeodeDevice::ReadbackStats afterTimeout = device->consumeReadbackStats();
  EXPECT_TRUE(afterTimeout.deviceLost);
  EXPECT_EQ(afterTimeout.timedOutWaitSite, GpuWaitSite::ReadbackMap);
  EXPECT_EQ(afterTimeout.timedOutWaitMs, static_cast<int>(kReadbackMapTimeout.count()));
}

/// Once a device hangs, every later bounded wait against it expires too. Those
/// are consequences, so the first attribution has to survive them or the
/// report names whichever wait happened to run last.
TEST(GeodeDeviceLost, FirstWaitTimeoutAttributionWins) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  device->markDeviceLostAfterWaitTimeout(GpuWaitSite::ReadbackMap, kReadbackMapTimeout,
                                         "test-injected readback map stall");
  device->markDeviceLostAfterWaitTimeout(GpuWaitSite::QueueIdle, kDefaultGpuWaitTimeout,
                                         "test-injected follow-on queue drain stall");

  const GeodeDevice::ReadbackStats stats = device->consumeReadbackStats();
  EXPECT_EQ(stats.timedOutWaitSite, GpuWaitSite::ReadbackMap);
  EXPECT_EQ(stats.timedOutWaitMs, static_cast<int>(kReadbackMapTimeout.count()));
}

/// A bounded wait for a submission serial that ends at its deadline has observed a device that
/// stopped answering, and must declare it lost with the same attribution the other bounded waits
/// record. Leaving the loss unpublished costs every later caller its own full budget on a device
/// that can no longer complete anything, and leaves a renderer unable to tell "slow" from "gone".
TEST(GeodeDeviceLost, RuntimeSerialWaitTimeoutDeclaresLossWithWaitAttribution) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);
  ASSERT_FALSE(device->isDeviceLost());

  gpu::Device& runtime = device->runtimeDevice();
  // One past the last serial this runtime submitted. Nothing can ever complete it, so the wait
  // has no outcome available to it other than reaching its own deadline.
  const uint64_t unreachableSerial = runtime.lastSubmittedSerial() + 1;
  EXPECT_THAT(runtime.waitForSerial(unreachableSerial, 0.25), testing::IsFalse());

  EXPECT_TRUE(device->isDeviceLost())
      << "a bounded runtime wait that reached its deadline must publish the loss it observed";

  const GeodeDevice::ReadbackStats stats = device->consumeReadbackStats();
  EXPECT_THAT(stats.timedOutWaitSite, Eq(GpuWaitSite::QueueIdle));
  EXPECT_GT(stats.timedOutWaitMs, 0);
}

/// A budget of zero is a question about what is already known rather than a wait, so its negative
/// answer says nothing about the device's health and must not declare it lost.
TEST(GeodeDeviceLost, RuntimeSerialWaitWithNoBudgetLeavesTheDeviceHealthy) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  gpu::Device& runtime = device->runtimeDevice();
  EXPECT_THAT(runtime.waitForSerial(runtime.lastSubmittedSerial() + 1, 0.0), testing::IsFalse());
  EXPECT_FALSE(device->isDeviceLost());
}

/// A driver-reported loss has no wait to attribute it to, and must not borrow
/// one: an empty site is how a report distinguishes "the driver told us" from
/// "one of our deadlines expired".
TEST(GeodeDeviceLost, DriverReportedLossHasNoWaitAttribution) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  device->markDeviceLost("test-injected driver-reported loss");

  const GeodeDevice::ReadbackStats stats = device->consumeReadbackStats();
  EXPECT_TRUE(stats.deviceLost);
  EXPECT_EQ(stats.timedOutWaitSite, GpuWaitSite::None);
  EXPECT_EQ(stats.timedOutWaitMs, 0);
}

/// The readback counters reset on every consume. The device-health fields must
/// not, or the frame after the failure would report a healthy device while
/// rendering stays dead.
TEST(GeodeDeviceLost, ReadbackStatsReportLossWithoutConsumingIt) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  device->recordReadback(/*usedTimedWaitAny=*/true, /*pollIterations=*/7);
  device->markDeviceLostAfterWaitTimeout(GpuWaitSite::QueueIdle, kDefaultGpuWaitTimeout,
                                         "test-injected queue drain stall");

  const GeodeDevice::ReadbackStats first = device->consumeReadbackStats();
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(first.pollIterations, 7);

  const GeodeDevice::ReadbackStats second = device->consumeReadbackStats();
  EXPECT_EQ(second.count, 0) << "readback counters must still be consumed";
  EXPECT_TRUE(second.deviceLost);
  EXPECT_EQ(second.timedOutWaitSite, GpuWaitSite::QueueIdle);
  EXPECT_EQ(second.timedOutWaitMs, static_cast<int>(kDefaultGpuWaitTimeout.count()));
}

/// A wait against a lost device must fail fast without polling the device:
/// the generous default timeout is seconds, so a fast return proves the
/// short-circuit rather than a lucky quick drain.
TEST(GeodeDeviceLost, WaitForQueueIdleFastFailsOnLostDevice) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  device->markDeviceLost("test-injected loss");
  const auto start = std::chrono::steady_clock::now();
  const GpuWaitResult result = device->waitForQueueIdle();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(result, GpuWaitResult::DeviceLost);
  EXPECT_LT(elapsed, std::chrono::seconds(1))
      << "lost-device wait must return immediately, not spend the timeout";
}

/// On a healthy device the bounded drain completes.
TEST(GeodeDeviceLost, WaitForQueueIdleCompletesOnHealthyDevice) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  // Submit a trivial command buffer so the wait has real work to drain.
  wgpu::CommandEncoder encoder = device->device().createCommandEncoder();
  wgpu::CommandBuffer cmd = encoder.finish();
  device->queue().submit(1, &cmd);

  EXPECT_EQ(device->waitForQueueIdle(), GpuWaitResult::Complete);
}

/// Teardown after a declared loss must complete without blocking on the GPU.
/// With submitted work still notionally in flight and the loss flag set, the
/// destructor must skip its queue drains; finishing well under the default
/// wait bound proves no bounded wait ran, and finishing at all proves no
/// unbounded wait ran.
TEST(GeodeDeviceLost, TeardownAfterLossSkipsGpuWaits) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  wgpu::CommandEncoder encoder = device->device().createCommandEncoder();
  wgpu::CommandBuffer cmd = encoder.finish();
  device->queue().submit(1, &cmd);
  device->markDeviceLost("test-injected loss before teardown");

  const auto start = std::chrono::steady_clock::now();
  device.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::seconds(1))
      << "post-loss teardown must not run bounded GPU waits";
}

/// An embedder-shared lost flag (GeodeEmbedConfig::lostState) is observed by
/// the wrapper, and a loss marked through the wrapper is visible to the
/// embedder: the flag converges both directions.
TEST(GeodeDeviceLost, ExternalConfigSharesLostState) {
  auto headless = GeodeDevice::CreateHeadless();
  ASSERT_NE(headless, nullptr);

  auto lostState = std::make_shared<GeodeDeviceLostState>();
  GeodeEmbedConfig config;
  config.device = headless->device();
  config.queue = headless->queue();
  config.lostState = lostState;
  auto external = GeodeDevice::CreateFromExternal(config);
  ASSERT_NE(external, nullptr);

  EXPECT_FALSE(external->isDeviceLost());
  lostState->lost.store(true, std::memory_order_release);
  EXPECT_TRUE(external->isDeviceLost());

  // Reset the shared flag and mark through the wrapper instead.
  lostState->lost.store(false, std::memory_order_release);
  external->markDeviceLost("wrapper-marked loss");
  EXPECT_TRUE(lostState->lost.load(std::memory_order_acquire));

  // The external wrapper's destructor must skip GPU waits (the flag is
  // set); destroy it before the headless owner so the underlying device
  // outlives the wrapper.
  external.reset();
}

}  // namespace donner::geode
