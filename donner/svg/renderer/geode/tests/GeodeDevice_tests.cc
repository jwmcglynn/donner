#include "donner/svg/renderer/geode/GeodeDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::geode {

using svg::test::RgbaEq;

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

/// Deferred-destroy queue: resources enqueued via deferDestroy() survive until
/// drainDeferredDestroys() is called, so an in-flight command buffer that
/// references them doesn't trigger a use-after-free.
TEST(GeodeDevice, DeferredDestroyBufferSurvivesUntilDrain) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  const wgpu::Device& device = geodeDevice->device();
  const wgpu::Queue& queue = geodeDevice->queue();

  // Create a buffer and write data to it, then defer its destruction.
  constexpr uint64_t kBufSize = 256;
  wgpu::BufferDescriptor desc = {};
  desc.label = wgpuLabel("DeferredDestroyTest");
  desc.size = kBufSize;
  desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer buffer = device.createBuffer(desc);
  ASSERT_TRUE(static_cast<bool>(buffer));

  // Write some data so the buffer is "in use".
  const uint32_t data[4] = {1, 2, 3, 4};
  queue.writeBuffer(buffer, 0, data, sizeof(data));

  // Move the buffer into the deferred-destroy queue. The wgpu handle is
  // internally reference-counted, so our local variable may still appear
  // "valid" after the move - what matters is that the deferred queue now
  // holds its own reference.
  geodeDevice->deferDestroy(std::move(buffer));

  // Submit an empty command buffer to create a GPU submission boundary.
  wgpu::CommandEncoder encoder = device.createCommandEncoder();
  wgpu::CommandBuffer cmdBuf = encoder.finish();
  queue.submit(1, &cmdBuf);

  // Drain: the buffer is now released. No wgpu validation errors should fire.
  geodeDevice->drainDeferredDestroys();

  // Submit another empty command buffer after drain to confirm no validation
  // errors from the destruction.
  wgpu::CommandEncoder encoder2 = device.createCommandEncoder();
  wgpu::CommandBuffer cmdBuf2 = encoder2.finish();
  queue.submit(1, &cmdBuf2);
}

/// Deferred-destroy queue: textures survive until drain.
TEST(GeodeDevice, DeferredDestroyTextureSurvivesUntilDrain) {
  auto geodeDevice = GeodeDevice::CreateHeadless();
  ASSERT_NE(geodeDevice, nullptr);

  const wgpu::Device& device = geodeDevice->device();
  const wgpu::Queue& queue = geodeDevice->queue();

  wgpu::TextureDescriptor desc = {};
  desc.label = wgpuLabel("DeferredDestroyTexTest");
  desc.size = {4, 4, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  desc.mipLevelCount = 1;
  desc.sampleCount = 1;
  desc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture texture = device.createTexture(desc);
  ASSERT_TRUE(static_cast<bool>(texture));

  // Use the texture as a render target, then defer destruction.
  wgpu::TextureView view = texture.createView();
  wgpu::CommandEncoder encoder = device.createCommandEncoder();
  wgpu::RenderPassColorAttachment color = {};
  color.view = view;
  color.loadOp = wgpu::LoadOp::Clear;
  color.storeOp = wgpu::StoreOp::Store;
  color.clearValue = {0.0, 1.0, 0.0, 1.0};
  wgpu::RenderPassDescriptor passDesc = {};
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &color;
  auto pass = encoder.beginRenderPass(passDesc);
  pass.end();
  wgpu::CommandBuffer cmdBuf = encoder.finish();
  queue.submit(1, &cmdBuf);

  geodeDevice->deferDestroy(std::move(texture));

  // Drain after submission - safe because wgpu internally refs submitted resources.
  geodeDevice->drainDeferredDestroys();

  // Verify no validation errors by submitting more work.
  wgpu::CommandEncoder encoder2 = device.createCommandEncoder();
  wgpu::CommandBuffer cmdBuf2 = encoder2.finish();
  queue.submit(1, &cmdBuf2);
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
