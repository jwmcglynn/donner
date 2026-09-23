/// @file
/// Tests for the Geode embedded-device code path.

#include "donner/svg/renderer/geode/GeodeEmbed.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"  // IWYU pragma: keep - provides wgpuLabel
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

using test::PixelAt;

constexpr double kViewportSize = 32.0;

using test::IsTransparent;

// ---------------------------------------------------------------------------
// GeodeDevice::CreateFromExternal
// ---------------------------------------------------------------------------

/// Create a headless device, then wrap it as if it were host-provided.
/// This exercises the CreateFromExternal factory without needing a real host
/// application.
TEST(GeodeEmbed, CreateFromExternalSucceeds) {
  auto headless = geode::GeodeDevice::CreateHeadless();
  ASSERT_NE(headless, nullptr);

  geode::GeodeEmbedConfig config;
  config.instance = headless->adapterDevice().root().instance();
  config.device = headless->adapterDevice().root().device();
  config.queue = headless->adapterDevice().root().queue();
  config.adapter = headless->adapterDevice().root().adapter();
  config.textureFormat = wgpu::TextureFormat::RGBA8Unorm;

  auto embedded = geode::GeodeDevice::CreateFromExternal(config);
  ASSERT_NE(embedded, nullptr);
  EXPECT_TRUE(static_cast<bool>(embedded->adapterDevice().root().device()));
  EXPECT_TRUE(static_cast<bool>(embedded->adapterDevice().root().queue()));
  EXPECT_TRUE(static_cast<bool>(embedded->adapterDevice().root().instance()));
  EXPECT_EQ(embedded->textureFormat(), gpu::TextureFormat::RGBA8Unorm);
}

/// Donner releases nothing an embedder lent it. A context built over a host's backend objects
/// borrows them, and when that context goes the host's instance, adapter, device and queue are
/// still the host's to render through. Releasing them here would take the GPU out from under a
/// host at a point the host never chose, and the symptom would surface in the host's own code
/// rather than in Donner's.
TEST(GeodeEmbed, DestroyingAnEmbeddedContextLeavesTheHostDeviceRendering) {
  std::shared_ptr<geode::GeodeDevice> host = geode::GeodeDevice::CreateHeadless();
  ASSERT_NE(host, nullptr);

  geode::GeodeEmbedConfig config;
  config.instance = host->adapterDevice().root().instance();
  config.device = host->adapterDevice().root().device();
  config.queue = host->adapterDevice().root().queue();
  config.adapter = host->adapterDevice().root().adapter();
  config.textureFormat = wgpu::TextureFormat::RGBA8Unorm;

  std::unique_ptr<geode::GeodeDevice> embedded = geode::GeodeDevice::CreateFromExternal(config);
  ASSERT_NE(embedded, nullptr);
  embedded.reset();

  ASSERT_FALSE(host->isDeviceLost())
      << "the host's device outlived a context that only borrowed it";

  // A full frame and readback through the host: it submits to the queue, waits on the serial and
  // maps the result back, so every object the embedded context borrowed is exercised rather than
  // only inspected.
  RendererGeode renderer(host);
  RenderViewport viewport;
  viewport.size = Vector2d(kViewportSize, kViewportSize);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  renderer.endFrame();

  const RendererBitmap snapshot = renderer.takeSnapshot();
  ASSERT_FALSE(snapshot.empty())
      << "the host could not complete a submission and readback after the context borrowing its "
         "backend objects was destroyed";
  EXPECT_THAT(PixelAt(snapshot, 16, 16), IsTransparent());
}

/// Null device should produce a null return, not a crash.
TEST(GeodeEmbed, CreateFromExternalRejectsNullDevice) {
  geode::GeodeEmbedConfig config;
  config.device = wgpu::Device();
  config.queue = wgpu::Queue();
  auto result = geode::GeodeDevice::CreateFromExternal(config);
  EXPECT_EQ(result, nullptr);
}

// ---------------------------------------------------------------------------
// RendererGeode with embedded device
// ---------------------------------------------------------------------------

class GeodeEmbedTest : public ::testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> sharedEmbedDevice() {
    static auto device = [] {
      // Create a real headless device, then wrap it via the embedded path so
      // all pipelines are created with the embedded factory.
      auto headless = geode::GeodeDevice::CreateHeadless();
      if (!headless) {
        return std::shared_ptr<geode::GeodeDevice>();
      }
      geode::GeodeEmbedConfig config;
      config.instance = headless->adapterDevice().root().instance();
      config.device = headless->adapterDevice().root().device();
      config.queue = headless->adapterDevice().root().queue();
      config.adapter = headless->adapterDevice().root().adapter();
      config.textureFormat = wgpu::TextureFormat::RGBA8Unorm;
      // Keep the headless device alive so the underlying wgpu objects persist.
      // The shared_ptr custom deleter captures `headless`.
      auto* raw = geode::GeodeDevice::CreateFromExternal(config).release();
      return std::shared_ptr<geode::GeodeDevice>(raw, [guard = std::move(headless)](auto* p) {
        delete p;
        (void)guard;
      });
    }();
    return device;
  }

  RendererGeode createRenderer() { return RendererGeode(sharedEmbedDevice()); }

  void beginFrame(RendererGeode& renderer) {
    RenderViewport viewport;
    viewport.size = Vector2d(kViewportSize, kViewportSize);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
  }
};

/// Empty frame through embedded device should produce a transparent bitmap.
TEST_F(GeodeEmbedTest, EmptyFrameIsTransparent) {
  auto renderer = createRenderer();
  beginFrame(renderer);
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());
  EXPECT_EQ(snap.dimensions.x, static_cast<int>(kViewportSize));
  EXPECT_EQ(snap.dimensions.y, static_cast<int>(kViewportSize));

  auto pixel = PixelAt(snap, 16, 16);
  EXPECT_THAT(pixel, IsTransparent()) << "Empty frame should be transparent";
}

// ---------------------------------------------------------------------------
// setTargetTexture
// ---------------------------------------------------------------------------

/// Render into a host-owned texture via setTargetTexture, then read it back.
/// Because the "host" target has CopySrc usage, takeSnapshot() should work.
TEST_F(GeodeEmbedTest, SetTargetTextureRendersIntoHostTexture) {
  auto device = sharedEmbedDevice();
  ASSERT_NE(device, nullptr);

  // Create a host-owned target texture.
  constexpr uint32_t kSize = 32;
  wgpu::TextureDescriptor texDesc = {};
  texDesc.label = geode::wgpuLabel("HostTarget");
  texDesc.size = {kSize, kSize, 1};
  texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                  wgpu::TextureUsage::TextureBinding;
  texDesc.mipLevelCount = 1;
  texDesc.sampleCount = 1;
  texDesc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture hostTexture = device->adapterDevice().root().device().createTexture(texDesc);
  ASSERT_TRUE(static_cast<bool>(hostTexture));
  // The host owns the texture; the renderer only ever names a texture of its own device, so the
  // host registers it and hands over the name.
  const gpu::Texture hostTarget =
      gpu::GetResultOrFail(device->adapterDevice().importExternalTexture(
          hostTexture, {kSize, kSize}, gpu::TextureFormat::RGBA8Unorm,
          gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
              gpu::TextureUsage::Sampled));

  auto renderer = createRenderer();
  renderer.setTargetTexture(hostTarget);

  // Render an empty frame - the target dimensions should come from the
  // host texture, not the viewport.
  RenderViewport viewport;
  viewport.size = Vector2d(kSize, kSize);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  renderer.endFrame();

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());
  EXPECT_EQ(snap.dimensions.x, static_cast<int>(kSize));
  EXPECT_EQ(snap.dimensions.y, static_cast<int>(kSize));

  auto pixel = PixelAt(snap, 16, 16);
  EXPECT_THAT(pixel, IsTransparent()) << "Empty frame should be transparent";

  renderer.clearTargetTexture();
}

/// A blend mode must not take the renderer down on a draw-only host target.
///
/// `mix-blend-mode` reads the parent's pixels back as a backdrop, which copies out of the frame
/// target. An embedder is free to hand over a surface it never asked to be copyable - the editor's
/// swapchain is exactly that unless framebuffer readback is turned on - and the blend mode comes
/// from the document, so the two meeting has to degrade to an unblended composite rather than
/// record a copy the runtime refuses.
TEST_F(GeodeEmbedTest, ABlendModeOnADrawOnlyHostTargetDegradesInsteadOfFailing) {
  auto device = sharedEmbedDevice();
  ASSERT_THAT(device, testing::NotNull());

  constexpr uint32_t kSize = 32;
  wgpu::TextureDescriptor texDesc = {};
  texDesc.label = geode::wgpuLabel("DrawOnlyHostTarget");
  texDesc.size = {kSize, kSize, 1};
  texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  texDesc.usage = wgpu::TextureUsage::RenderAttachment;  // No CopySrc, no TextureBinding.
  texDesc.mipLevelCount = 1;
  texDesc.sampleCount = 1;
  texDesc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture hostTexture = device->adapterDevice().root().device().createTexture(texDesc);
  ASSERT_THAT(static_cast<bool>(hostTexture), testing::IsTrue());
  const gpu::Texture hostTarget =
      gpu::GetResultOrFail(device->adapterDevice().importExternalTexture(
          hostTexture, {kSize, kSize}, gpu::TextureFormat::RGBA8Unorm,
          gpu::TextureUsage::RenderAttachment));

  auto renderer = createRenderer();
  renderer.setTargetTexture(hostTarget);

  RenderViewport viewport;
  viewport.size = Vector2d(kSize, kSize);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  renderer.pushIsolatedLayer(1.0, MixBlendMode::Multiply);
  renderer.drawRect(Box2d({0, 0}, {kSize, kSize}), StrokeParams{});
  renderer.popIsolatedLayer();
  renderer.endFrame();

  EXPECT_THAT(renderer.deviceLost(), testing::IsFalse())
      << "A blend mode the target cannot serve must cost the blend, not the device";
  EXPECT_THAT(renderer.takeSnapshot().empty(), testing::IsTrue())
      << "A target the embedder did not make copyable cannot be read back";

  renderer.clearTargetTexture();
}

/// After clearTargetTexture, the renderer goes back to internal targets.
TEST_F(GeodeEmbedTest, ClearTargetTextureRevertsToInternal) {
  auto renderer = createRenderer();

  // First render with a host target.
  auto device = sharedEmbedDevice();
  wgpu::TextureDescriptor texDesc = {};
  texDesc.label = geode::wgpuLabel("HostTarget2");
  texDesc.size = {16, 16, 1};
  texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                  wgpu::TextureUsage::TextureBinding;
  texDesc.mipLevelCount = 1;
  texDesc.sampleCount = 1;
  texDesc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture hostTexture = device->adapterDevice().root().device().createTexture(texDesc);
  const gpu::Texture hostTarget =
      gpu::GetResultOrFail(device->adapterDevice().importExternalTexture(
          hostTexture, {16, 16}, gpu::TextureFormat::RGBA8Unorm,
          gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
              gpu::TextureUsage::Sampled));
  renderer.setTargetTexture(hostTarget);

  RenderViewport viewport;
  viewport.size = Vector2d(16, 16);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  renderer.endFrame();
  EXPECT_EQ(renderer.width(), 16);
  EXPECT_EQ(renderer.height(), 16);

  // Clear the host target and render a 32×32 frame.
  renderer.clearTargetTexture();
  viewport.size = Vector2d(32, 32);
  renderer.beginFrame(viewport);
  renderer.endFrame();
  EXPECT_EQ(renderer.width(), 32);
  EXPECT_EQ(renderer.height(), 32);

  RendererBitmap snap = renderer.takeSnapshot();
  ASSERT_FALSE(snap.empty());
  EXPECT_EQ(snap.dimensions.x, 32);
}

/// A target the renderer's device does not own is not a surface it can draw into: the handle
/// names a slot on another device, and everything the frame records would resolve it against a
/// table where that slot means something else. The frame is declined rather than recorded.
TEST_F(GeodeEmbedTest, ATargetOfAnotherDeviceIsRefused) {
  auto device = sharedEmbedDevice();
  ASSERT_NE(device, nullptr);
  const std::unique_ptr<geode::GeodeDevice> elsewhere = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(elsewhere, testing::NotNull())
      << "Failed to create a second headless wgpu device. Check driver availability.";

  constexpr uint32_t kSize = 32;
  wgpu::TextureDescriptor texDesc = {};
  texDesc.label = geode::wgpuLabel("TargetOfAnotherDevice");
  texDesc.size = {kSize, kSize, 1};
  texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
  texDesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
                  wgpu::TextureUsage::TextureBinding;
  texDesc.mipLevelCount = 1;
  texDesc.sampleCount = 1;
  texDesc.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture foreignTexture = elsewhere->adapterDevice().root().device().createTexture(texDesc);
  ASSERT_THAT(static_cast<bool>(foreignTexture), testing::IsTrue());
  // Registered with the device that allocated it, which is the only device that can name it.
  const gpu::Texture foreignTarget =
      gpu::GetResultOrFail(elsewhere->adapterDevice().importExternalTexture(
          foreignTexture, {kSize, kSize}, gpu::TextureFormat::RGBA8Unorm,
          gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
              gpu::TextureUsage::Sampled));

  auto renderer = createRenderer();
  renderer.setTargetTexture(foreignTarget);

  RenderViewport viewport;
  viewport.size = Vector2d(kSize, kSize);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  renderer.endFrame();

  EXPECT_THAT(renderer.lastFrameTimings().counters.submits, testing::Eq(0u))
      << "a frame whose target belongs to another device must be declined, not recorded";

  renderer.clearTargetTexture();

  // The same frame against a target of the renderer's own device, so a submission count that is
  // zero for every target cannot pass the assertion above.
  texDesc.label = geode::wgpuLabel("TargetOfThisDevice");
  wgpu::Texture ownTexture = device->adapterDevice().root().device().createTexture(texDesc);
  ASSERT_THAT(static_cast<bool>(ownTexture), testing::IsTrue());
  const gpu::Texture ownTarget = gpu::GetResultOrFail(device->adapterDevice().importExternalTexture(
      ownTexture, {kSize, kSize}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
          gpu::TextureUsage::Sampled));
  renderer.setTargetTexture(ownTarget);
  renderer.beginFrame(viewport);
  renderer.endFrame();
  EXPECT_THAT(renderer.lastFrameTimings().counters.submits, testing::Gt(0u))
      << "a frame whose target is this device's own must be recorded";
  renderer.clearTargetTexture();
}

}  // namespace
}  // namespace donner::svg
