/// @file
/// Tests for the Geode embedded-device code path.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"  // IWYU pragma: keep - provides wgpuLabel
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

constexpr double kViewportSize = 32.0;

using test::IsTransparent;

/// RGBA pixel at (x, y) in a tightly packed snapshot bitmap.
std::array<uint8_t, 4> pixelAt(const RendererBitmap& bitmap, int x, int y) {
  const size_t off = static_cast<size_t>(y) * bitmap.rowBytes + static_cast<size_t>(x) * 4u;
  return {bitmap.pixels[off], bitmap.pixels[off + 1], bitmap.pixels[off + 2],
          bitmap.pixels[off + 3]};
}

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
  config.instance = headless->instance();
  config.device = headless->device();
  config.queue = headless->queue();
  config.adapter = headless->adapter();
  config.textureFormat = wgpu::TextureFormat::RGBA8Unorm;

  auto embedded = geode::GeodeDevice::CreateFromExternal(config);
  ASSERT_NE(embedded, nullptr);
  EXPECT_TRUE(static_cast<bool>(embedded->device()));
  EXPECT_TRUE(static_cast<bool>(embedded->queue()));
  EXPECT_TRUE(static_cast<bool>(embedded->instance()));
  EXPECT_EQ(embedded->textureFormat(), wgpu::TextureFormat::RGBA8Unorm);
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
      config.instance = headless->instance();
      config.device = headless->device();
      config.queue = headless->queue();
      config.adapter = headless->adapter();
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

  auto pixel = pixelAt(snap, 16, 16);
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
  wgpu::Texture hostTexture = device->device().createTexture(texDesc);
  ASSERT_TRUE(static_cast<bool>(hostTexture));

  auto renderer = createRenderer();
  renderer.setTargetTexture(hostTexture);

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

  auto pixel = pixelAt(snap, 16, 16);
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
  wgpu::Texture hostTexture = device->device().createTexture(texDesc);
  ASSERT_THAT(static_cast<bool>(hostTexture), testing::IsTrue());

  auto renderer = createRenderer();
  renderer.setTargetTexture(hostTexture);

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
  wgpu::Texture hostTexture = device->device().createTexture(texDesc);
  renderer.setTargetTexture(hostTexture);

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

}  // namespace
}  // namespace donner::svg
