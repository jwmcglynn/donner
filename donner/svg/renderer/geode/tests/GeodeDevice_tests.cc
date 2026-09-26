#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/SharingTestDevice.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/tests/GeodeTestContexts.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

#if defined(__APPLE__)
#include "donner/gpu/metal/MetalDevice.h"
#endif
#if defined(__linux__)
#include "donner/gpu/vulkan/VulkanDevice.h"
#endif

namespace donner::geode {

using svg::test::RgbaEq;
using testing::ElementsAreArray;
using testing::Eq;
using testing::Ge;
using testing::IsFalse;
using testing::IsNull;
using testing::IsTrue;
using testing::Lt;
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

/// Smoke test: can we instantiate a headless device at all, on the backend the process selects?
/// If this fails, the entire Geode backend is non-functional.
TEST(GeodeDevice, CreateHeadlessSucceeds) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr) << "Failed to create a headless device. Check driver availability "
                                "(Metal on macOS, Vulkan/SwiftShader on Linux).";

  EXPECT_TRUE(device->physicalDeviceOwner()->root().hasBackendDevice());
}

TEST(GeodeDevice, SecondLogicalContextUsesSelectedPhysicalOwnerWithoutEmbedHandles) {
  auto first = GeodeDevice::CreateHeadless();
  ASSERT_THAT(first, NotNull());
  const std::shared_ptr<GeodePhysicalDeviceOwner> owner = first->physicalDeviceOwner();
  ASSERT_THAT(owner, NotNull());

  auto second = GeodeDevice::CreateOverPhysicalDeviceOwner(owner, gpu::TextureFormat::RGBA8Unorm);
  ASSERT_THAT(second, NotNull());
  EXPECT_THAT(second->physicalDeviceOwner(), Eq(owner));
  EXPECT_THAT(second->textureFormat(), Eq(gpu::TextureFormat::RGBA8Unorm));
  EXPECT_NE(second->runtimeDevice().deviceId(), first->runtimeDevice().deviceId());
}

TEST(GeodeDevice, SecondLogicalContextRejectsNullOrLostPhysicalOwner) {
  EXPECT_THAT(GeodeDevice::CreateOverPhysicalDeviceOwner(nullptr, gpu::TextureFormat::BGRA8Unorm),
              IsNull());
  auto first = GeodeDevice::CreateHeadless();
  ASSERT_THAT(first, NotNull());
  const std::shared_ptr<GeodePhysicalDeviceOwner> owner = first->physicalDeviceOwner();
  ASSERT_THAT(owner, NotNull());
  owner->lostState()->lost.store(true, std::memory_order_release);
  EXPECT_THAT(GeodeDevice::CreateOverPhysicalDeviceOwner(owner, gpu::TextureFormat::BGRA8Unorm),
              IsNull());
}

/// An offscreen render target holds exactly the texels written to it. Every texel of a 64x64
/// pattern comes back through a mapped readback buffer, so the allocation has the extent and format
/// it was described with, and the readback buffer a snapshot readback allocates can be created and
/// mapped, on the backend the process selects.
TEST(GeodeDevice, ARenderTargetRoundTripsTheTexelsWrittenToIt) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  gpu::Device& runtime = device->runtimeDevice();
  constexpr uint32_t kSize = 64;
  const gpu::Texture target = gpu::GetResultOrFail(runtime.createTexture(
      gpu::TextureDescriptor{"TestRenderTarget",
                             {kSize, kSize},
                             gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
                                 gpu::TextureUsage::CopyDst}));
  std::vector<uint8_t> written(static_cast<size_t>(kSize) * kSize * 4u);
  for (size_t i = 0; i < written.size(); ++i) {
    written[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
  }
  ASSERT_THAT(
      runtime.writeTexture(target, written, gpu::TexelCopyBufferLayout{0, kSize * 4u, kSize},
                           gpu::Extent2d{kSize, kSize}),
      gpu::IsOk());

  const gpu::Result<std::vector<uint8_t>> readBack =
      ReadTexturePixels(runtime, target, gpu::Extent2d{kSize, kSize});
  ASSERT_THAT(readBack, gpu::HasResult());
  EXPECT_THAT(readBack.result(), ElementsAreArray(written));
}

/// End-to-end: clear a texture to red and read back the first pixel.
/// This proves that command submission and texture readback actually work.
TEST(GeodeDevice, CanExecuteClearAndReadback) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);
  gpu::Device& runtime = geodeDevice->runtimeDevice();

  constexpr uint32_t kSize = 4;  // Small texture for a quick test.
  const gpu::Texture target = gpu::GetResultOrFail(runtime.createTexture(
      gpu::TextureDescriptor{"ClearTarget",
                             {kSize, kSize},
                             gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc}));
  const gpu::TextureView targetView = gpu::GetResultOrFail(
      runtime.createTextureView(target, gpu::TextureViewDescriptor{"ClearTargetView"}));

  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(runtime.createCommandEncoder());
  gpu::RenderPassEncoder* pass =
      gpu::GetResultOrFail(encoder->beginRenderPass(gpu::RenderPassDescriptor{
          "ClearToRed",
          {gpu::RenderPassColorAttachment{
              targetView, gpu::LoadOp::Clear, gpu::StoreOp::Store, {1.0, 0.0, 0.0, 1.0}}}}));
  ASSERT_THAT(pass->end(), gpu::IsOk());
  ASSERT_THAT(runtime.submit(gpu::GetResultOrFail(encoder->finish())), gpu::HasResult());

  const gpu::Result<std::vector<uint8_t>> pixels =
      ReadTexturePixels(runtime, target, gpu::Extent2d{kSize, kSize});
  ASSERT_THAT(pixels, gpu::HasResult());
  const std::vector<uint8_t>& bytes = pixels.result();
  const std::array<uint8_t, 4> firstPixel = {bytes[0], bytes[1], bytes[2], bytes[3]};
  EXPECT_THAT(firstPixel, RgbaEq(255, 0, 0, 255));
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
/// Native backends return no valid runtime pipeline for a rejected descriptor.
TEST(GeodeDevice, BatchedFillPipelineCompilesWithoutValidationErrors) {
  auto device = GeodeDevice::CreateHeadless();
  ASSERT_NE(device, nullptr);

  const gpu::RenderPipeline& batched = device->pipeline().batchedPipeline();

  ASSERT_TRUE(batched.isValid()) << "Native batched fill pipeline creation failed.";

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
  ASSERT_THAT(SubmitEmptyCommandBuffer(device->runtimeDevice()), testing::Gt(0u));

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

  ASSERT_THAT(SubmitEmptyCommandBuffer(device->runtimeDevice()), testing::Gt(0u));
  device->markDeviceLost("test-injected loss before teardown");

  const auto start = std::chrono::steady_clock::now();
  device.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::seconds(1))
      << "post-loss teardown must not run bounded GPU waits";
}

/// Milliseconds elapsed since \p start, as an integer a failure message prints.
/// @param start When the measured span began.
int64_t MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
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

TEST(GeodeNativeMetalRoot, SecondLogicalContextSharesPhysicalOwnerWithoutWebGpuEmbedConfig) {
  std::unique_ptr<GeodeDevice> first = CreateNativeMetalContext();
  ASSERT_THAT(first, NotNull()) << kNoMetalDevice;
  const std::shared_ptr<GeodePhysicalDeviceOwner> owner = first->physicalDeviceOwner();
  ASSERT_THAT(owner, NotNull());
  ASSERT_THAT(owner->root().capabilities().backend, Eq(GpuBackendKind::NativeMetal));

  std::unique_ptr<GeodeDevice> second =
      GeodeDevice::CreateOverPhysicalDeviceOwner(owner, gpu::TextureFormat::RGBA8Unorm);
  ASSERT_THAT(second, NotNull());
  EXPECT_THAT(second->physicalDeviceOwner(), Eq(owner));
  EXPECT_NE(second->runtimeDevice().deviceId(), first->runtimeDevice().deviceId());
  EXPECT_THAT(second->textureFormat(), Eq(gpu::TextureFormat::RGBA8Unorm));
}

/// A native root reports the texture limit its device reports, not the 8,192-texel fallback a
/// root uses when it has no device to ask. The renderer refuses images, filter regions and layers
/// past this limit, so a root that reports the fallback on a device that allocates more drops
/// content the device can draw. The device also allocates a texture at the limit its root reports.
TEST(GeodeNativeMetalRoot, ReportsTheTextureLimitItsDeviceReports) {
  std::unique_ptr<GeodeDevice> context = CreateNativeMetalContext();
  ASSERT_THAT(context, NotNull()) << kNoMetalDevice;
  const std::optional<gpu::metal::MetalDevice::SystemCapabilities> device =
      gpu::metal::MetalDevice::QuerySystemCapabilities();
  ASSERT_TRUE(device.has_value()) << kNoMetalDevice;

  const uint32_t limit = context->maxTextureDimension2D();
  EXPECT_THAT(limit, Eq(device->maxTextureDimension2D))
      << "the root reports a texture limit other than the one its Metal device reports";
  EXPECT_THAT(
      context->runtimeDevice().createTexture(gpu::TextureDescriptor{"AtTheReportedLimit",
                                                                    {limit, 1},
                                                                    gpu::TextureFormat::RGBA8Unorm,
                                                                    gpu::TextureUsage::Sampled}),
      gpu::HasResult())
      << "the device refused a texture at the limit its root reports, " << limit << " texels";
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
  // Well under the 5 s default budget, so a wait that spends a budget other than the caller's
  // fails here rather than passing.
  EXPECT_THAT(elapsedMs, Lt(kBudget.count() + 1000))
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

/// A loss declared while the drain waits is reported as that loss rather than as the drain's own
/// timeout: the drain did not observe the device stop answering, so it must not claim it did.
/// The submission stays held, and the loss is declared from another thread partway through the
/// wait, as a driver-reported loss on the shared root would be.
TEST(GeodeNativeMetalRoot, QueueIdleReportsALossDeclaredDuringTheWait) {
  std::unique_ptr<GeodeDevice> context = CreateNativeMetalContext();
  ASSERT_THAT(context, NotNull()) << kNoMetalDevice;
  auto& metal = static_cast<gpu::metal::MetalDevice&>(context->runtimeDevice());
  ASSERT_THAT(metal.pauseSubmissionsForTest(), gpu::IsOk());
  const uint64_t submitted = SubmitEmptyCommandBuffer(metal);
  ASSERT_THAT(submitted, testing::Gt(0u));

  // The loss comes early in a long budget, so a loaded host that delays the declaring thread
  // still declares it well before the drain could give up on its own.
  constexpr std::chrono::milliseconds kBudget(1500);
  std::thread declarer([&context] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    context->markDeviceLost("test-injected loss during a queue drain");
  });
  const auto start = std::chrono::steady_clock::now();
  const GpuWaitResult result = context->waitForQueueIdle(kBudget);
  const int64_t elapsedMs = MillisecondsSince(start);
  declarer.join();

  EXPECT_THAT(result, Eq(GpuWaitResult::DeviceLost))
      << "a loss declared during the drain is reported as the loss, not as a drain timeout";
  EXPECT_THAT(elapsedMs, Lt(kBudget.count() + 1000))
      << "the drain outlived its own budget after the loss was declared";
  EXPECT_THAT(context->consumeReadbackStats().timedOutWaitSite, Eq(GpuWaitSite::None))
      << "the drain must not attribute a loss it did not observe to its own deadline";

  metal.resumeSubmissionsForTest();
}

#endif  // defined(__APPLE__)

#if defined(__linux__)

/// Why a native Vulkan case could not start: the host has no Vulkan device to select.
constexpr const char* kNoVulkanDevice = "no Vulkan device is available on this host";

/// A context over a native Vulkan root, selected by name for the same reason as the native Metal
/// cases above.
std::unique_ptr<GeodeDevice> CreateNativeVulkanContext() {
  GpuRootSelection selection;
  selection.label = "GeodeNativeVulkanTest";
  selection.backend = GpuBackendKind::NativeVulkan;
  std::shared_ptr<GeodeGpuRoot> root = SelectGpuRoot(selection);
  if (root == nullptr) {
    return nullptr;
  }
  return GeodeDevice::CreateOverSelectedRoot(std::move(root), gpu::TextureFormat::RGBA8Unorm);
}

TEST(GeodeNativeVulkanRoot, HeadlessSelectionDoesNotExposePresentationInstance) {
  std::unique_ptr<GeodeDevice> context = CreateNativeVulkanContext();
  ASSERT_THAT(context, NotNull()) << kNoVulkanDevice;
  auto& device = static_cast<gpu::vulkan::VulkanDevice&>(context->runtimeDevice());
  EXPECT_FALSE(device.supportsPresentation());
  EXPECT_THAT(device.nativeInstance(), IsNull());
}

TEST(GeodeNativeVulkanRoot, PresentationSelectionSharesInstanceAcrossRuntimeDevices) {
  GpuRootSelection selection;
  selection.label = "GeodeNativeVulkanPresentationTest";
  selection.backend = GpuBackendKind::NativeVulkan;
  selection.requireVulkanPresentation = true;
  std::shared_ptr<GeodeGpuRoot> root = SelectGpuRoot(selection);
  if (root == nullptr) {
    const char* required = std::getenv("DONNER_REQUIRE_VULKAN");
    if (required != nullptr && std::string_view(required) == "1") {
      ASSERT_THAT(CreateNativeVulkanContext(), NotNull()) << kNoVulkanDevice;
    }
    GTEST_SKIP() << "This Vulkan loader or driver lacks the surface and swapchain extensions "
                    "needed for a presentation-capable shared root";
  }
  void* instance = root->vulkanRoot()->nativeInstance();
  ASSERT_THAT(instance, NotNull());

  GeodeRuntimeDevice first = CreateGpuDeviceOver(root);
  GeodeRuntimeDevice second = CreateGpuDeviceOver(root);
  ASSERT_THAT(first.device, NotNull());
  ASSERT_THAT(second.device, NotNull());
  auto& firstNative = static_cast<gpu::vulkan::VulkanDevice&>(*first.device);
  auto& secondNative = static_cast<gpu::vulkan::VulkanDevice&>(*second.device);
  EXPECT_TRUE(firstNative.supportsPresentation());
  EXPECT_TRUE(secondNative.supportsPresentation());
  EXPECT_THAT(firstNative.nativeInstance(), Eq(instance));
  EXPECT_THAT(secondNative.nativeInstance(), Eq(instance));
  EXPECT_THAT(firstNative.nativeContextForTest().queue,
              Eq(secondNative.nativeContextForTest().queue));
}

TEST(GeodeNativeVulkanRoot, PresentationSelectionRejectsMissingRequiredExtension) {
  const std::array<const char*, 1> required{"VK_EXT_donner_missing"};
  GpuRootSelection selection;
  selection.backend = GpuBackendKind::NativeVulkan;
  selection.requireVulkanPresentation = true;
  selection.requiredVulkanInstanceExtensions = required;
  EXPECT_THAT(SelectGpuRoot(selection), IsNull());
}

TEST(GeodeNativeVulkanRoot, AdoptedRootRequiresTheExactLossOwner) {
  auto loss = std::make_shared<gpu::DeviceLostState>();
  std::shared_ptr<gpu::vulkan::VulkanSharedRoot> native =
      gpu::vulkan::VulkanDevice::CreateSharedRoot(loss);
  ASSERT_THAT(native, NotNull()) << kNoVulkanDevice;
  EXPECT_THAT(AdoptNativeVulkanRoot(nullptr, loss), IsNull());
  EXPECT_THAT(AdoptNativeVulkanRoot(native, nullptr), IsNull());
  EXPECT_THAT(AdoptNativeVulkanRoot(native, std::make_shared<gpu::DeviceLostState>()), IsNull());

  std::shared_ptr<GeodeGpuRoot> root = AdoptNativeVulkanRoot(native, loss);
  ASSERT_THAT(root, NotNull());
  EXPECT_THAT(root->vulkanRoot(), Eq(native));
  EXPECT_THAT(root->lostState(), Eq(loss));
  EXPECT_THAT(root->capabilities().backend, Eq(GpuBackendKind::NativeVulkan));
  EXPECT_THAT(root->capabilities().maxTextureDimension2D, native->maxTextureDimension2D());
}

/// Runtime devices over one selected Vulkan root share its native device and queue, while each
/// retains its own submission serials and survives a sibling's teardown.
TEST(GeodeNativeVulkanRoot, RuntimeDevicesShareOneNativeDeviceAndIndependentSerials) {
  std::unique_ptr<GeodeDevice> context = CreateNativeVulkanContext();
  ASSERT_THAT(context, NotNull()) << kNoVulkanDevice;
  GeodeRuntimeDevice sibling = context->physicalDeviceOwner()->createLogicalDevice();
  ASSERT_THAT(sibling.device, NotNull()) << kNoVulkanDevice;
  auto& first = static_cast<gpu::vulkan::VulkanDevice&>(context->runtimeDevice());
  auto& second = static_cast<gpu::vulkan::VulkanDevice&>(*sibling.device);
  const auto firstNative = first.nativeContextForTest();
  const auto secondNative = second.nativeContextForTest();
  ASSERT_THAT(firstNative.device, NotNull());
  ASSERT_THAT(secondNative.device, NotNull());
  EXPECT_THAT(secondNative.instance, Eq(firstNative.instance));
  EXPECT_THAT(secondNative.device, Eq(firstNative.device));
  EXPECT_THAT(secondNative.queue, Eq(firstNative.queue));
  EXPECT_THAT(secondNative.queueFamilyIndex, Eq(firstNative.queueFamilyIndex));
  const gpu::Texture firstTexture = gpu::GetResultOrFail(first.createTexture(gpu::TextureDescriptor{
      "FirstOnly", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled}));
  EXPECT_THAT(second.createTextureView(firstTexture, gpu::TextureViewDescriptor{"ForeignView"}),
              gpu::IsGpuError(gpu::GpuErrorType::DeviceMismatch))
      << "sharing the native VkDevice must not merge the runtime handle tables";

  const uint64_t firstBefore = first.lastSubmittedSerial();
  const uint64_t secondBefore = second.lastSubmittedSerial();
  const uint64_t firstSerial = SubmitEmptyCommandBuffer(first);
  EXPECT_THAT(firstSerial, Eq(firstBefore + 1u));
  EXPECT_THAT(second.lastSubmittedSerial(), Eq(secondBefore));
  EXPECT_THAT(SubmitEmptyCommandBuffer(second), Eq(secondBefore + 1u));
  EXPECT_THAT(first.lastSubmittedSerial(), Eq(firstSerial));

  sibling.device.reset();
  EXPECT_THAT(SubmitEmptyCommandBuffer(first), Eq(firstSerial + 1u))
      << "one device's teardown must not close the shared root under its sibling";
}

/// A runtime device retains the native root after the context that selected it has gone away.
TEST(GeodeNativeVulkanRoot, ASecondDeviceSurvivesItsSelectingContextsDestruction) {
  std::unique_ptr<GeodeDevice> context = CreateNativeVulkanContext();
  ASSERT_THAT(context, NotNull()) << kNoVulkanDevice;
  GeodeRuntimeDevice sibling = context->physicalDeviceOwner()->createLogicalDevice();
  ASSERT_THAT(sibling.device, NotNull()) << kNoVulkanDevice;
  auto& second = static_cast<gpu::vulkan::VulkanDevice&>(*sibling.device);
  const auto nativeDevice = second.nativeContextForTest().device;
  context.reset();

  EXPECT_THAT(second.nativeContextForTest().device, Eq(nativeDevice));
  const uint64_t serial = SubmitEmptyCommandBuffer(second);
  ASSERT_THAT(serial, testing::Gt(0u));
  EXPECT_TRUE(second.waitForSerial(serial, 5.0))
      << "the surviving device's own fence must still complete after root selection is gone";
}

/// Each runtime device stays on its own thread while native submissions share one Vulkan queue.
TEST(GeodeNativeVulkanRoot, ConcurrentRuntimeDevicesSubmitThroughTheSharedQueue) {
  std::shared_ptr<gpu::vulkan::VulkanSharedRoot> root =
      gpu::vulkan::VulkanDevice::CreateSharedRoot();
  ASSERT_THAT(root, NotNull()) << kNoVulkanDevice;
  constexpr uint64_t kSubmissions = 32;
  struct WorkerResult {
    bool opened = false;
    bool submitted = false;
    bool completed = false;
  };
  std::array<WorkerResult, 2> results{};
  std::mutex gateMutex;
  std::condition_variable gateChanged;
  int ready = 0;
  bool go = false;
  const auto run = [&](size_t index) {
    std::unique_ptr<gpu::vulkan::VulkanDevice> device =
        gpu::vulkan::VulkanDevice::CreateOverSharedRoot(root);
    {
      std::unique_lock lock(gateMutex);
      results[index].opened = device != nullptr;
      ++ready;
      gateChanged.notify_all();
      gateChanged.wait(lock, [&] { return go; });
    }
    if (device == nullptr) {
      return;
    }
    bool submitted = true;
    for (uint64_t expected = 1; expected <= kSubmissions; ++expected) {
      if (SubmitEmptyCommandBuffer(*device) != expected) {
        submitted = false;
        break;
      }
    }
    results[index].submitted = submitted;
    if (submitted) {
      results[index].completed = device->waitForSerial(kSubmissions, 5.0);
    }
  };
  std::thread first(run, 0);
  std::thread second(run, 1);
  bool bothReady;
  {
    std::unique_lock lock(gateMutex);
    bothReady = gateChanged.wait_for(lock, std::chrono::seconds(5), [&] { return ready == 2; });
    go = true;
    gateChanged.notify_all();
  }
  first.join();
  second.join();
  EXPECT_TRUE(bothReady);
  for (size_t index = 0; index < results.size(); ++index) {
    SCOPED_TRACE(::testing::Message() << "worker " << index);
    EXPECT_THAT(results[index].opened, IsTrue());
    EXPECT_THAT(results[index].submitted, IsTrue());
    EXPECT_THAT(results[index].completed, IsTrue());
  }
}

/// A native Vulkan root reports the texture limit its physical device reports rather than the
/// 8,192-texel fallback, for the reason given for the native Metal root, and the device allocates
/// a texture at that limit.
TEST(GeodeNativeVulkanRoot, ReportsTheTextureLimitItsDeviceReports) {
  std::unique_ptr<GeodeDevice> context = CreateNativeVulkanContext();
  ASSERT_THAT(context, NotNull()) << kNoVulkanDevice;
  const std::optional<gpu::vulkan::VulkanDevice::SystemCapabilities> device =
      gpu::vulkan::VulkanDevice::QuerySystemCapabilities();
  ASSERT_TRUE(device.has_value()) << kNoVulkanDevice;
  // Vulkan requires every implementation to allocate at least this much.
  EXPECT_THAT(device->maxTextureDimension2D, Ge(4096u));

  const uint32_t limit = context->maxTextureDimension2D();
  EXPECT_THAT(limit, Eq(device->maxTextureDimension2D))
      << "the root reports a texture limit other than the one its Vulkan device reports";
  EXPECT_THAT(
      context->runtimeDevice().createTexture(gpu::TextureDescriptor{"AtTheReportedLimit",
                                                                    {limit, 1},
                                                                    gpu::TextureFormat::RGBA8Unorm,
                                                                    gpu::TextureUsage::Sampled}),
      gpu::HasResult())
      << "the device refused a texture at the limit its root reports, " << limit << " texels";
}

/// On a native backend the queue is idle exactly when the last submission has retired, so a drain
/// after ordinary work completes and declares nothing.
TEST(GeodeNativeVulkanRoot, QueueIdleCompletesOnceTheLastSubmissionRetires) {
  std::unique_ptr<GeodeDevice> context = CreateNativeVulkanContext();
  ASSERT_THAT(context, NotNull()) << kNoVulkanDevice;
  const uint64_t submitted = SubmitEmptyCommandBuffer(context->runtimeDevice());
  ASSERT_THAT(submitted, testing::Gt(0u));

  EXPECT_THAT(context->waitForQueueIdle(), Eq(GpuWaitResult::Complete));
  EXPECT_THAT(context->runtimeDevice().completedSerial(), Ge(submitted))
      << "the wait reported the queue idle before its last submission retired";
  EXPECT_FALSE(context->isDeviceLost()) << "a drain that completed observed nothing to declare";
}

/// A loss the driver reports is the driver's report, not a queue drain that gave up. After a
/// submission fails with device loss, the context and every other device over its root report the
/// loss, and the drain returns it at once without recording a queue-idle timeout.
TEST(GeodeNativeVulkanRoot, QueueIdleReportsADriverReportedLossWithoutATimeout) {
  std::unique_ptr<GeodeDevice> context = CreateNativeVulkanContext();
  ASSERT_THAT(context, NotNull()) << kNoVulkanDevice;
  GeodeRuntimeDevice sibling = context->physicalDeviceOwner()->createLogicalDevice();
  ASSERT_THAT(sibling.device, NotNull()) << kNoVulkanDevice;
  // The root selected the native backend, so its runtime device is the Vulkan device.
  auto& vulkan = static_cast<gpu::vulkan::VulkanDevice&>(context->runtimeDevice());
  vulkan.failNextSubmissionForTest(/*deviceLost=*/true);
  ASSERT_THAT(SubmitEmptyCommandBuffer(vulkan), Eq(0u))
      << "the submission the driver refused with VK_ERROR_DEVICE_LOST must fail";

  constexpr std::chrono::milliseconds kBudget(1500);
  const auto start = std::chrono::steady_clock::now();
  const GpuWaitResult result = context->waitForQueueIdle(kBudget);
  const int64_t elapsedMs = MillisecondsSince(start);

  EXPECT_THAT(result, Eq(GpuWaitResult::DeviceLost))
      << "a drain after a driver-reported loss must report the loss, not a timeout";
  EXPECT_THAT(elapsedMs, Lt(kBudget.count()))
      << "the drain spent its budget on a device the driver had already reported lost";
  EXPECT_TRUE(context->isDeviceLost());
  EXPECT_TRUE(sibling.device->isLost()) << "the loss belongs to the root, not to one device";
  EXPECT_THAT(context->consumeReadbackStats().timedOutWaitSite, Eq(GpuWaitSite::None))
      << "the driver reported this loss; the drain must not attribute it to its own deadline";
}

#endif  // defined(__linux__)

/// Two runtime devices on separate queues whose loss conditions are their own, as over an embedder
/// root with private loss states, so every declaration shows up on exactly the device it names.
class OrderedRegistrationTest : public testing::Test {
protected:
  gpu::FakeNativeDevice native_;
  std::shared_ptr<gpu::DeviceLostState> producerLost_ = std::make_shared<gpu::DeviceLostState>();
  std::shared_ptr<gpu::DeviceLostState> consumerLost_ = std::make_shared<gpu::DeviceLostState>();
  gpu::SharingDevice producer_{native_, gpu::SharingOptions{.lostState = producerLost_}};
  gpu::SharingDevice consumer_{native_, gpu::SharingOptions{.lostState = consumerLost_}};
};

/// A producer already lost is refused at registration, and nothing is declared on the consumer.
TEST_F(OrderedRegistrationTest, AnAlreadyLostProducerIsRefusedWithoutDeclaringTheConsumer) {
  const gpu::Texture owned = gpu::MakeSharedTexture(producer_);
  const gpu::TextureExport exported = gpu::GetResultOrFail(producer_.exportTexture(owned));
  ASSERT_TRUE(gpu::DeclareDeviceLost(*producerLost_));

  EXPECT_THAT(RegisterOrderedTexture(consumer_, exported),
              gpu::IsGpuError(gpu::GpuErrorType::DeviceLost));
  EXPECT_THAT(consumer_.isLost(), IsFalse());
}

/// A producer whose work failed ends the wait at once. The failure is the producer's: the source
/// wait declares it on the producer with no wait site, and the helper fails with `DeviceLost`
/// without writing the consumer's condition, least of all with a wait site no deadline produced.
TEST_F(OrderedRegistrationTest, AProducerThatFailsIsNotDeclaredOnTheConsumer) {
  const gpu::Texture owned = gpu::MakeSharedTexture(producer_);
  producer_.holdCompletion();
  ASSERT_THAT(gpu::SubmitSharedTextureRead(producer_, owned), gpu::HasResult());
  const gpu::TextureExport exported = gpu::GetResultOrFail(producer_.exportTexture(owned));
  producer_.failExecution();

  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(RegisterOrderedTexture(consumer_, exported),
              gpu::IsGpuError(gpu::GpuErrorType::DeviceLost));
  EXPECT_THAT(std::chrono::steady_clock::now() - start, Lt(std::chrono::seconds(1)));
  EXPECT_THAT(producerLost_->lost.load(), IsTrue());
  EXPECT_THAT(producerLost_->timedOutSite.load(), Eq(gpu::DeviceLostWaitSite::None));
  EXPECT_THAT(consumer_.isLost(), IsFalse())
      << "the consumer's wait never reached its bound, so it has no hang to report";
  EXPECT_THAT(consumerLost_->timedOutSite.load(), Eq(gpu::DeviceLostWaitSite::None));
}

/// A wait that spends its whole bound is the one failure the helper treats as a hang: the
/// producer's queue stopped answering, so the consumer's condition is declared lost with the
/// queue-idle site and the wait that actually ran, and the producer's condition is left to its own
/// waits.
TEST_F(OrderedRegistrationTest, AWaitThatSpendsItsWholeBoundDeclaresAQueueIdleTimeout) {
  const gpu::Texture owned = gpu::MakeSharedTexture(producer_);
  producer_.holdCompletion();
  ASSERT_THAT(gpu::SubmitSharedTextureRead(producer_, owned), gpu::HasResult());
  const gpu::TextureExport exported = gpu::GetResultOrFail(producer_.exportTexture(owned));

  constexpr std::chrono::milliseconds kBound{20};
  EXPECT_THAT(RegisterOrderedTexture(consumer_, exported, kBound),
              gpu::IsGpuError(gpu::GpuErrorType::DeviceLost));
  EXPECT_THAT(consumerLost_->timedOutSite.load(), Eq(gpu::DeviceLostWaitSite::QueueIdle));
  EXPECT_THAT(consumerLost_->timedOutElapsedMs.load(), Ge(kBound.count()));
  EXPECT_THAT(producer_.isLost(), IsFalse());
}

/// A registration the backend orders on the device needs no host wait: between contexts over one
/// root, with the producer's work still running, the helper returns the registration at once and
/// declares nothing, because the consumer's submissions wait for that work on the device instead.
TEST(DeviceOrderedRegistrationTest, TheHelperReturnsWithoutWaitingForTheProducersWork) {
  gpu::FakeNativeDevice native;
  const auto rootLoss = std::make_shared<gpu::DeviceLostState>();
  gpu::SharingDevice producer(
      native,
      gpu::SharingOptions{.ordering = gpu::SourceOrdering::WaitOnDevice, .lostState = rootLoss});
  gpu::SharingDevice consumer(
      native,
      gpu::SharingOptions{.ordering = gpu::SourceOrdering::WaitOnDevice, .lostState = rootLoss});
  const gpu::Texture owned = gpu::MakeSharedTexture(producer);
  producer.holdCompletion();
  ASSERT_THAT(gpu::SubmitSharedTextureRead(producer, owned), gpu::HasResult());
  const gpu::TextureExport exported = gpu::GetResultOrFail(producer.exportTexture(owned));

  const auto start = std::chrono::steady_clock::now();
  const gpu::Result<gpu::Texture> registered = RegisterOrderedTexture(consumer, exported);
  const auto spent = std::chrono::steady_clock::now() - start;

  EXPECT_THAT(registered, gpu::HasResult());
  EXPECT_THAT(std::chrono::duration_cast<std::chrono::milliseconds>(spent),
              Lt(kDefaultGpuWaitTimeout / 5))
      << "the helper waited on the host for work the device orders";
  EXPECT_THAT(consumer.isLost(), IsFalse());
  EXPECT_THAT(producer.isLost(), IsFalse());
}

}  // namespace donner::geode
