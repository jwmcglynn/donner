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
#include <thread>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeEmbed.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

#if defined(__APPLE__)
#include "donner/gpu/metal/MetalDevice.h"
#endif

namespace donner::geode {

using svg::test::RgbaEq;
using testing::Eq;
using testing::Ge;
using testing::HasSubstr;
using testing::IsNull;
using testing::Lt;
using testing::Not;
using testing::NotNull;

/// Submits one empty command buffer through \p runtime and returns the serial it was given, or 0
/// when the runtime refused any step of it. A wait for a serial needs a serial that was really
/// submitted: a device that accepted work and stopped retiring it is the only thing such a wait
/// can be waiting on.
/// @param runtime Runtime device to submit through.
uint64_t SubmitEmptyCommandBuffer(gpu::Device& runtime) {
  gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoder = runtime.createCommandEncoder();
  if (encoder.hasError()) {
    return 0;
  }
  gpu::Result<gpu::CommandBuffer> commands = encoder.result()->finish();
  if (commands.hasError()) {
    return 0;
  }
  gpu::Result<uint64_t> serial = runtime.submit(std::move(commands).result());
  return serial.hasError() ? 0 : serial.result();
}

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

  EXPECT_TRUE(static_cast<bool>(device->adapterDevice().root().device()));
  EXPECT_TRUE(static_cast<bool>(device->adapterDevice().root().queue()));
  EXPECT_TRUE(static_cast<bool>(device->adapterDevice().root().adapter()));
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
      [](GeodeEmbedConfig& config, GeodeDevice& foreign) {
        config.device = foreign.adapterDevice().root().device();
      }},
     {"queue", [](GeodeEmbedConfig& config,
                  GeodeDevice& foreign) { config.queue = foreign.adapterDevice().root().queue(); }},
     {"instance",
      [](GeodeEmbedConfig& config, GeodeDevice& foreign) {
        config.instance = foreign.adapterDevice().root().instance();
      }},
     {"adapter",
      [](GeodeEmbedConfig& config, GeodeDevice& foreign) {
        config.adapter = foreign.adapterDevice().root().adapter();
      }},
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
  ASSERT_THAT(static_cast<WGPUAdapter>(foreignContext->adapterDevice().root().adapter()), NotNull())
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
  agreeing.device = ownerContext->adapterDevice().root().device();
  agreeing.queue = ownerContext->adapterDevice().root().queue();
  agreeing.instance = ownerContext->adapterDevice().root().instance();
  agreeing.adapter = ownerContext->adapterDevice().root().adapter();
  EXPECT_THAT(GeodeDevice::CreateFromExternal(agreeing), NotNull())
      << "CreateFromExternal refused a config that repeats its own physical owner's roots";
}

TEST(GeodeDevice, LegacyBorrowedAggregateConfigurationRemainsSupported) {
  auto ownerContext = GeodeDevice::CreateHeadless();
  ASSERT_NE(ownerContext, nullptr);

  GeodeEmbedConfig config{ownerContext->adapterDevice().root().instance(),
                          ownerContext->adapterDevice().root().device(),
                          ownerContext->adapterDevice().root().queue(),
                          wgpu::TextureFormat::RGBA8Unorm,
                          ownerContext->adapterDevice().root().adapter(),
                          std::make_shared<GeodeDeviceLostState>()};
  EXPECT_NE(GeodeDevice::CreateFromExternal(config), nullptr);
}

TEST(GeodeDevice, SharedPhysicalOwnerRejectsAlreadyLostDevice) {
  auto ownerContext = GeodeDevice::CreateHeadless();
  ASSERT_NE(ownerContext, nullptr);

  auto lostState = std::make_shared<GeodeDeviceLostState>();
  GeodeEmbedConfig borrowed;
  borrowed.instance = ownerContext->adapterDevice().root().instance();
  borrowed.adapter = ownerContext->adapterDevice().root().adapter();
  borrowed.device = ownerContext->adapterDevice().root().device();
  borrowed.queue = ownerContext->adapterDevice().root().queue();
  borrowed.lostState = lostState;
  auto borrowedContext = GeodeDevice::CreateFromExternal(borrowed);
  ASSERT_NE(borrowedContext, nullptr);
  lostState->lost.store(true, std::memory_order_release);

  GeodeEmbedConfig config;
  config.physicalDevice = borrowedContext->physicalDeviceOwner();
  EXPECT_EQ(GeodeDevice::CreateFromExternal(config), nullptr);

  // A lost root skips every teardown wait, so let the borrowed context go before the headless
  // owner whose backend objects it names.
  borrowedContext.reset();
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

  wgpu::Texture texture = device->adapterDevice().root().device().createTexture(desc);
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

  wgpu::Buffer buffer = device->adapterDevice().root().device().createBuffer(desc);
  ASSERT_TRUE(static_cast<bool>(buffer));
  EXPECT_EQ(buffer.getSize(), 1024u);
}

/// End-to-end: clear a texture to red and read back the first pixel.
/// This proves that command submission and texture readback actually work.
TEST(GeodeDevice, CanExecuteClearAndReadback) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  const wgpu::Device& device = geodeDevice->adapterDevice().root().device();
  const wgpu::Queue& queue = geodeDevice->adapterDevice().root().queue();

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

/// Budget the deadline cases below give a wait, chosen so the measured elapsed time is
/// unambiguously the budget rather than scheduling noise, and the case still runs in well under a
/// second.
constexpr double kSerialWaitBudgetSeconds = 0.25;

/// A bounded wait for a submission serial that spends its whole budget without the work retiring
/// has observed a device that stopped answering, and must declare it lost with the same
/// attribution the other bounded waits record. Leaving the loss unpublished costs every later
/// caller its own full budget on a device that can no longer complete anything, and leaves a
/// renderer unable to tell "slow" from "gone".
TEST(GeodeDeviceLost, RuntimeSerialWaitTimeoutDeclaresLossWithWaitAttribution) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);
  ASSERT_FALSE(device->isDeviceLost());

  GeodeWgpuAdapterDevice& runtime = device->adapterDevice();
  const uint64_t submitted = SubmitEmptyCommandBuffer(runtime);
  ASSERT_THAT(submitted, testing::Gt(0u));
  // Submitted work that stops retiring, on a device whose poll blocks the way a driver waiting on
  // it does: the wait can only end by spending its budget.
  runtime.holdSubmittedWorkForTesting(submitted - 1, std::chrono::milliseconds(1));

  EXPECT_THAT(runtime.waitForSerial(submitted, kSerialWaitBudgetSeconds), testing::IsFalse());

  EXPECT_TRUE(device->isDeviceLost())
      << "a bounded runtime wait that spent its deadline must publish the loss it observed";

  const GeodeDevice::ReadbackStats stats = device->consumeReadbackStats();
  EXPECT_THAT(stats.timedOutWaitSite, Eq(GpuWaitSite::QueueIdle));
  EXPECT_THAT(stats.timedOutWaitMs, Ge(static_cast<int>(kSerialWaitBudgetSeconds * 1000.0) - 1))
      << "the loss has to come from a deadline that actually elapsed, so the attribution reports "
         "a wait that was really spent";

  runtime.holdSubmittedWorkForTesting(GeodeWgpuAdapterDevice::kNoCompletedSerialCeiling,
                                      std::chrono::milliseconds(0));
}

/// A wait that ends by exhausting its own poll bound has spent none of its budget: the driver
/// returned from every poll without blocking and without progressing. That says something about
/// how this driver implements poll, not that submitted work stopped completing, and declaring a
/// permanent loss from it fails every later caller on a device that is merely idle.
TEST(GeodeDeviceLost, RuntimeSerialWaitExhaustingOnlyItsPollBoundLeavesTheDeviceHealthy) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);
  ASSERT_FALSE(device->isDeviceLost());

  GeodeWgpuAdapterDevice& runtime = device->adapterDevice();
  const uint64_t submitted = SubmitEmptyCommandBuffer(runtime);
  ASSERT_THAT(submitted, testing::Gt(0u));
  // The same work held incomplete, but a poll that returns at once. The budget is far past
  // anything this wait can spend, so only the poll bound can end it.
  runtime.holdSubmittedWorkForTesting(submitted - 1, std::chrono::milliseconds(0));

  constexpr double kUnreachableBudgetSeconds = 120.0;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(runtime.waitForSerial(submitted, kUnreachableBudgetSeconds), testing::IsFalse());
  const double elapsedSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  // Bounded well under the per-case budget rather than merely under the wait's: the poll bound is
  // reached in milliseconds when poll returns without blocking, which is the only shape this case
  // is about, and a wait that took seconds here ended some other way.
  ASSERT_THAT(elapsedSeconds, Lt(5.0))
      << "the poll bound, not the deadline, has to be what ended this wait";
  EXPECT_FALSE(device->isDeviceLost())
      << "a wait that spent none of its budget observed nothing to declare";

  runtime.holdSubmittedWorkForTesting(GeodeWgpuAdapterDevice::kNoCompletedSerialCeiling,
                                      std::chrono::milliseconds(0));
}

/// A budget of zero is a question about what is already known rather than a wait, so its negative
/// answer says nothing about the device's health and must not declare it lost.
TEST(GeodeDeviceLost, RuntimeSerialWaitWithNoBudgetLeavesTheDeviceHealthy) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  GeodeWgpuAdapterDevice& runtime = device->adapterDevice();
  const uint64_t submitted = SubmitEmptyCommandBuffer(runtime);
  ASSERT_THAT(submitted, testing::Gt(0u));
  runtime.holdSubmittedWorkForTesting(submitted - 1, std::chrono::milliseconds(0));

  EXPECT_THAT(runtime.waitForSerial(submitted, 0.0), testing::IsFalse());
  EXPECT_FALSE(device->isDeviceLost())
      << "a question about what is already known is not a wait, so its negative answer is no "
         "evidence that the device stopped answering";

  runtime.holdSubmittedWorkForTesting(GeodeWgpuAdapterDevice::kNoCompletedSerialCeiling,
                                      std::chrono::milliseconds(0));
}

/// Teardown drains what it submitted so deferred destructions can run, and tolerates its own
/// timeout: wgpu retains every resource a submitted command buffer names until it completes, so
/// an overrun is a slow teardown rather than a discovery. It is also routine - a loaded host, a
/// software rasterizer, a contended driver - and the contexts sharing this root are still live.
/// Declaring the root lost from it would make every one of them refuse to present, map or wait,
/// and would leak the whole root, because a root declared lost is deliberately not released.
TEST(GeodeDeviceLost, ATeardownDrainThatOverrunsDoesNotDeclareTheRootLost) {
  auto context = GeodeDevice::CreateHeadless();
  ASSERT_NE(context, nullptr);
  ASSERT_FALSE(context->isDeviceLost());

  std::unique_ptr<gpu::Device> siblingRuntime =
      context->physicalDeviceOwner()->createLogicalDevice();
  ASSERT_NE(siblingRuntime, nullptr);
  ASSERT_TRUE(context->hasTransitionalAdapter());
  std::unique_ptr<GeodeWgpuAdapterDevice> sibling(
      static_cast<GeodeWgpuAdapterDevice*>(siblingRuntime.release()));
  const uint64_t submitted = SubmitEmptyCommandBuffer(*sibling);
  ASSERT_THAT(submitted, testing::Gt(0u));
  sibling->holdSubmittedWorkForTesting(submitted - 1, std::chrono::milliseconds(1));
  sibling->setTeardownDrainBudgetForTesting(0.2);

  sibling.reset();

  EXPECT_FALSE(context->isDeviceLost())
      << "only a wait a caller bounded for its own deadline has observed something worth "
         "publishing; a teardown drain already proceeds on timeout";
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
  wgpu::CommandEncoder encoder = device->adapterDevice().root().device().createCommandEncoder();
  wgpu::CommandBuffer cmd = encoder.finish();
  device->adapterDevice().root().queue().submit(1, &cmd);

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

  wgpu::CommandEncoder encoder = device->adapterDevice().root().device().createCommandEncoder();
  wgpu::CommandBuffer cmd = encoder.finish();
  device->adapterDevice().root().queue().submit(1, &cmd);
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
  config.device = headless->adapterDevice().root().device();
  config.queue = headless->adapterDevice().root().queue();
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

#if defined(__APPLE__)

/// Why a native Metal case could not start: the host has no Metal device to select.
constexpr const char* kNoMetalDevice = "no Metal device is available on this host";

/// A context over a native Metal root. These cases are about that backend, so they select it by
/// name rather than taking whatever the process selects by default.
std::unique_ptr<GeodeDevice> CreateNativeMetalContext() {
  GpuRootSelection selection;
  selection.label = "GeodeNativeMetalTest";
  selection.backend = GpuBackendKind::NativeMetal;
  std::shared_ptr<GeodeGpuRoot> root = SelectGpuRoot(selection);
  if (root == nullptr) {
    return nullptr;
  }
  return GeodeDevice::CreateOverSelectedRoot(std::move(root), gpu::TextureFormat::RGBA8Unorm);
}

/// Milliseconds elapsed since \p start, as an integer a failure message prints.
/// @param start When the measured span began.
int64_t MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

/// The texture limit a native root reports is what its devices allocate. The renderer refuses
/// images, filter regions and layers past this limit, so a root that reports less than its
/// device allocates drops content the device can draw, and one that reports more hands the device
/// work it refuses. The limit therefore has to hold from both sides: a texture at the limit is
/// allocated and one a texel past it is refused as over the limit.
TEST(GeodeNativeMetalRoot, ReportsTheLargestTextureItsDevicesAllocate) {
  std::unique_ptr<GeodeDevice> context = CreateNativeMetalContext();
  ASSERT_THAT(context, NotNull()) << kNoMetalDevice;

  const uint32_t limit = context->maxTextureDimension2D();
  gpu::Device& runtime = context->runtimeDevice();
  EXPECT_THAT(runtime.createTexture(gpu::TextureDescriptor{"AtTheReportedLimit",
                                                           {limit, 1},
                                                           gpu::TextureFormat::RGBA8Unorm,
                                                           gpu::TextureUsage::Sampled}),
              gpu::HasResult())
      << "the device refused a texture at the limit its root reports, " << limit << " texels";
  EXPECT_THAT(runtime.createTexture(gpu::TextureDescriptor{"PastTheReportedLimit",
                                                           {limit + 1, 1},
                                                           gpu::TextureFormat::RGBA8Unorm,
                                                           gpu::TextureUsage::Sampled}),
              gpu::IsGpuError(gpu::GpuErrorType::LimitExceeded))
      << "the device allocated a texture past the limit its root reports, " << limit
      << " texels, so the renderer refuses content the device can draw";
}

/// A native backend has no poll that reports an empty queue: the queue is idle exactly when the
/// last submission has retired. Held work is the case that tells a real wait apart from one that
/// returns early, and the bound is what keeps a driver that stopped answering from costing more
/// than one deadline. Spending the budget is the observation the wait was there to make, so it is
/// published as a loss attributed to the queue drain, as the transitional adapter's drain does.
TEST(GeodeNativeMetalRoot, QueueIdleOnHeldWorkSpendsItsBudgetThenDeclaresTheLoss) {
  std::unique_ptr<GeodeDevice> context = CreateNativeMetalContext();
  ASSERT_THAT(context, NotNull()) << kNoMetalDevice;
  // The root selected the native backend, so its runtime device is the Metal device.
  auto& metal = static_cast<gpu::metal::MetalDevice&>(context->runtimeDevice());
  ASSERT_THAT(metal.pauseSubmissionsForTest(), gpu::IsOk());
  const uint64_t submitted = SubmitEmptyCommandBuffer(metal);
  ASSERT_THAT(submitted, testing::Gt(0u));

  constexpr std::chrono::milliseconds kBudget(200);
  const auto start = std::chrono::steady_clock::now();
  const GpuWaitResult result = context->waitForQueueIdle(kBudget);
  const int64_t elapsedMs = MillisecondsSince(start);

  EXPECT_THAT(result, Eq(GpuWaitResult::TimedOut))
      << "the last submission was held incomplete, so reporting the queue idle means the wait "
         "returned before the work retired";
  EXPECT_THAT(metal.completedSerial(), Lt(submitted))
      << "the held submission retired anyway, so this case observed no held work";
  EXPECT_THAT(elapsedMs, Ge(kBudget.count())) << "the wait gave up before its budget was spent";
  EXPECT_THAT(elapsedMs, Lt(kBudget.count() + 5000))
      << "the wait outlived its budget, so a driver that stopped answering costs more than one "
         "deadline";
  EXPECT_TRUE(context->isDeviceLost()) << "a drain that spent its deadline must publish the loss";
  EXPECT_THAT(context->consumeReadbackStats().timedOutWaitSite, Eq(GpuWaitSite::QueueIdle));

  metal.resumeSubmissionsForTest();
}

/// The drain returns once the last submission retires and not before. The submission is held
/// when the wait starts and released from another thread while it is under way, so a wait that
/// reports the queue idle while the work is still held is visible in the completed serial at the
/// moment it returns. The seam's release only signals a shared event, which Metal allows from any
/// thread, and the waiting thread reads nothing but completion state meanwhile.
TEST(GeodeNativeMetalRoot, QueueIdleReturnsOnceTheLastSubmissionRetires) {
  std::unique_ptr<GeodeDevice> context = CreateNativeMetalContext();
  ASSERT_THAT(context, NotNull()) << kNoMetalDevice;
  auto& metal = static_cast<gpu::metal::MetalDevice&>(context->runtimeDevice());
  ASSERT_THAT(metal.pauseSubmissionsForTest(), gpu::IsOk());
  const uint64_t submitted = SubmitEmptyCommandBuffer(metal);
  ASSERT_THAT(submitted, testing::Gt(0u));

  std::thread releaser([&metal] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    metal.resumeSubmissionsForTest();
  });
  const GpuWaitResult result = context->waitForQueueIdle();
  const uint64_t completedAtReturn = metal.completedSerial();
  releaser.join();

  EXPECT_THAT(result, Eq(GpuWaitResult::Complete));
  EXPECT_THAT(completedAtReturn, Ge(submitted))
      << "the wait reported the queue idle while its last submission was still held";
  EXPECT_FALSE(context->isDeviceLost()) << "a drain that completed observed nothing to declare";
}

#endif  // defined(__APPLE__)

}  // namespace donner::geode
