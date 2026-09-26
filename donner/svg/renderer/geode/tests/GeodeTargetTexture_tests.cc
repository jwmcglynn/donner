/// @file
/// Renderer targets supplied by a host context through the native GPU runtime.

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
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

using test::PixelAt;

constexpr double kViewportSize = 32.0;

using test::IsTransparent;

class GeodeTargetTextureTest : public ::testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
    return device;
  }

  RendererGeode createRenderer() { return RendererGeode(sharedDevice()); }

  void beginFrame(RendererGeode& renderer) {
    RenderViewport viewport;
    viewport.size = Vector2d(kViewportSize, kViewportSize);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
  }
};

/// Empty frame through a caller-shared device should produce a transparent bitmap.
TEST_F(GeodeTargetTextureTest, EmptyFrameIsTransparent) {
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

/// Render into a caller-supplied runtime texture and read back its pixels.
TEST_F(GeodeTargetTextureTest, SetTargetTextureRendersIntoRuntimeTexture) {
  auto device = sharedDevice();
  ASSERT_NE(device, nullptr);

  // The host controls the target's lifetime and usage, while this device owns its allocation.
  constexpr uint32_t kSize = 32;
  const gpu::Texture hostTarget = gpu::GetResultOrFail(device->runtimeDevice().createTexture(
      {"HostTarget",
       {kSize, kSize},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
           gpu::TextureUsage::Sampled}));

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
TEST_F(GeodeTargetTextureTest, ABlendModeOnADrawOnlyTargetDegradesInsteadOfFailing) {
  auto device = sharedDevice();
  ASSERT_THAT(device, testing::NotNull());

  constexpr uint32_t kSize = 32;
  const gpu::Texture hostTarget = gpu::GetResultOrFail(
      device->runtimeDevice().createTexture({"DrawOnlyHostTarget",
                                             {kSize, kSize},
                                             gpu::TextureFormat::RGBA8Unorm,
                                             gpu::TextureUsage::RenderAttachment}));

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
TEST_F(GeodeTargetTextureTest, ClearTargetTextureRevertsToInternal) {
  auto renderer = createRenderer();

  // First render with a caller-supplied target.
  auto device = sharedDevice();
  const gpu::Texture hostTarget = gpu::GetResultOrFail(device->runtimeDevice().createTexture(
      {"HostTarget2",
       {16, 16},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
           gpu::TextureUsage::Sampled}));
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
TEST_F(GeodeTargetTextureTest, ATargetOfAnotherDeviceIsRefused) {
  auto device = sharedDevice();
  ASSERT_NE(device, nullptr);
  const std::unique_ptr<geode::GeodeDevice> elsewhere = geode::GeodeDevice::CreateHeadless();
  ASSERT_THAT(elsewhere, testing::NotNull())
      << "Failed to create a second native GPU device. Check driver availability.";

  constexpr uint32_t kSize = 32;
  const gpu::Texture foreignTarget = gpu::GetResultOrFail(elsewhere->runtimeDevice().createTexture(
      {"TargetOfAnotherDevice",
       {kSize, kSize},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
           gpu::TextureUsage::Sampled}));

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
  const gpu::Texture ownTarget = gpu::GetResultOrFail(device->runtimeDevice().createTexture(
      {"TargetOfThisDevice",
       {kSize, kSize},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
           gpu::TextureUsage::Sampled}));
  renderer.setTargetTexture(ownTarget);
  renderer.beginFrame(viewport);
  renderer.endFrame();
  EXPECT_THAT(renderer.lastFrameTimings().counters.submits, testing::Gt(0u))
      << "a frame whose target is this device's own must be recorded";
  renderer.clearTargetTexture();
}

}  // namespace
}  // namespace donner::svg
