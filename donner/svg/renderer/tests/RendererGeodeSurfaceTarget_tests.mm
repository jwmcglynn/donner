/// @file
/// Captures of a renderer whose target is the frame a surface has out, over a real Core Animation
/// layer: the platform object and the surface frame the editor presents through.

#import <QuartzCore/CAMetalLayer.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "donner/base/Box.h"
#include "donner/css/Color.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

using test::PixelAt;
using test::RgbaEq;
using testing::IsTrue;

/// Frame size, in pixels.
constexpr uint32_t kFrameSize = 64;

/// A surface over a fresh Core Animation layer whose frames the renderer can draw into and copy
/// from.
class RendererGeodeSurfaceTargetTest : public testing::Test {
protected:
  void SetUp() override {
    // Core Animation presents BGRA8, so the renderer's device is created for that format.
    device_ = geode::GeodeDevice::CreateHeadless(gpu::TextureFormat::BGRA8Unorm);
    ASSERT_NE(device_, nullptr) << "no GPU device is available on this host";
    layer_ = [CAMetalLayer layer];
    layer_.contentsScale = 1.0;

    gpu::SurfaceDescriptor descriptor;
    descriptor.label = "window";
    descriptor.native.kind = gpu::NativeSurfaceKind::MetalLayer;
    descriptor.native.display = (__bridge void*)layer_;
    surface_ = gpu::GetResultOrFail(runtime().createSurface(descriptor));
    ASSERT_THAT(runtime().configureSurface(
                    surface_,
                    gpu::SurfaceConfiguration{
                        gpu::TextureFormat::BGRA8Unorm,
                        gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc,
                        gpu::Extent2d{kFrameSize, kFrameSize}, gpu::PresentMode::Fifo,
                        gpu::SurfaceAlphaMode::Opaque}),
                gpu::IsOk());
  }

  gpu::Device& runtime() { return device_->runtimeDevice(); }

  /// Draws opaque red over the whole of \p frame and captures it.
  /// @param renderer Renderer over this device. @param frame Frame the surface has out.
  static RendererBitmap drawRedAndCapture(RendererGeode& renderer, const gpu::Texture& frame) {
    renderer.setTargetTexture(frame);
    RenderViewport viewport;
    viewport.size = Vector2d(kFrameSize, kFrameSize);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    PaintParams paint;
    paint.fill = PaintServer::Solid{css::Color(css::RGBA(255, 0, 0, 255))};
    renderer.setPaint(paint);
    renderer.drawRect(Box2d({0, 0}, {kFrameSize, kFrameSize}), StrokeParams{});
    renderer.endFrame();
    RendererBitmap bitmap = renderer.takeSnapshot();
    renderer.clearTargetTexture();
    return bitmap;
  }

  std::shared_ptr<geode::GeodeDevice> device_;
  CAMetalLayer* layer_ = nil;
  gpu::Surface surface_;
};

/// `setTargetTexture` promises that a target with `CopySrc` can be read back, and a surface frame
/// is such a target. Where the capture context shares the renderer's queue, its readback is queued
/// before the frame is presented, so the capture reads the frame the renderer drew. A native
/// backend gives the capture context its own queue, whose read could land after the present, so
/// there the capture is refused and comes back empty.
TEST_F(RendererGeodeSurfaceTargetTest, ACaptureOfASurfaceFrameReadsItWhereTheQueueIsShared) {
  // The renderer outlives the present, as a presenting host's does.
  RendererGeode renderer(device_);
  gpu::SurfaceTexture frame = gpu::GetResultOrFail(runtime().acquireCurrentTexture(surface_));
  ASSERT_THAT(frame.texture.isValid(), IsTrue());

  const RendererBitmap bitmap = drawRedAndCapture(renderer, frame.texture);
  if (device_->hasTransitionalAdapter()) {
    ASSERT_FALSE(bitmap.empty()) << "the capture refused the surface frame the renderer drew";
    EXPECT_THAT(PixelAt(bitmap, kFrameSize / 2, kFrameSize / 2), RgbaEq(255, 0, 0, 255));
  } else {
    EXPECT_TRUE(bitmap.empty())
        << "a capture on its own queue could read the frame after the surface takes it back";
  }
  EXPECT_THAT(runtime().presentSurface(surface_), gpu::HasResult());
  EXPECT_FALSE(device_->isDeviceLost());
}

}  // namespace
}  // namespace donner::svg
