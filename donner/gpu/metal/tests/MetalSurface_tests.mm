/// @file
/// The Metal presentation slice: a Core Animation layer configured through the runtime's surface
/// hooks, drawables acquired and drawn into, and frames presented, abandoned, and released.
///
/// The layer is offscreen. Core Animation hands out drawables from a layer that was never added
/// to a view, which is what lets presentation be tested without a window; what a compositor does
/// with a presented frame is outside this runtime either way.

#import <QuartzCore/CAMetalLayer.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal::tests {
namespace {

using testing::Contains;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::HasSubstr;

/// Rows of a texel copy are 256-byte aligned, so every extent below is a multiple of 64 texels
/// wide and can be read back without a padded staging step.
constexpr uint32_t kSurfaceWidth = 64;
constexpr uint32_t kSurfaceHeight = 48;

/// Opaque red, premultiplied RGBA. Every channel is 0 or 1 so the expected bytes are exact.
constexpr std::array<double, 4> kRedClear = {1.0, 0.0, 0.0, 1.0};
/// The same color as BGRA8 texels, the one format a Core Animation layer presents.
constexpr std::array<uint8_t, 4> kRedBgra = {0, 0, 255, 255};

class MetalSurfaceTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "the Metal presentation slice");

    layer_ = [CAMetalLayer layer];
    layer_.contentsScale = 1.0;
  }

  /// Unwraps an RHI result, failing the test on error.
  template <typename T>
  T unwrap(Result<T>&& result, const char* what) {
    if (result.hasError()) {
      ADD_FAILURE() << what << " failed: " << result.error();
    }
    return std::move(result).result();
  }

  SurfaceDescriptor surfaceDescriptor() const {
    SurfaceDescriptor descriptor;
    descriptor.label = "presentation";
    descriptor.native.kind = NativeSurfaceKind::MetalLayer;
    descriptor.native.display = (__bridge void*)layer_;
    return descriptor;
  }

  static SurfaceConfiguration configuration(uint32_t width = kSurfaceWidth,
                                            uint32_t height = kSurfaceHeight) {
    return SurfaceConfiguration{
        TextureFormat::BGRA8Unorm, TextureUsage::RenderAttachment | TextureUsage::CopySrc,
        Extent2d{width, height}, PresentMode::Fifo, SurfaceAlphaMode::Opaque};
  }

  Surface configuredSurface(uint32_t width = kSurfaceWidth, uint32_t height = kSurfaceHeight) {
    Surface surface = unwrap(device_->createSurface(surfaceDescriptor()), "createSurface");
    EXPECT_THAT(device_->configureSurface(surface, configuration(width, height)), IsOk());
    return surface;
  }

  /// Clears \p frame to \p clearColor, optionally copying the result into \p readback, and
  /// returns the submission serial.
  uint64_t renderClear(const Texture& frame, std::array<double, 4> clearColor,
                       const Buffer* readback, uint32_t width, uint32_t height) {
    TextureView view = unwrap(device_->createTextureView(frame, TextureViewDescriptor{"frame"}),
                              "createTextureView");
    std::unique_ptr<CommandEncoder> encoder =
        unwrap(device_->createCommandEncoder(), "createCommandEncoder");

    RenderPassDescriptor pass;
    pass.label = "present";
    pass.colorAttachments.push_back(
        RenderPassColorAttachment{view, LoadOp::Clear, StoreOp::Store, clearColor});
    RenderPassEncoder* renderPass = unwrap(encoder->beginRenderPass(pass), "beginRenderPass");
    EXPECT_THAT(renderPass->end(), IsOk());

    if (readback != nullptr) {
      EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{frame}, *readback,
                                               TexelCopyBufferLayout{0, width * 4u, height},
                                               Extent2d{width, height}),
                  IsOk());
    }

    CommandBuffer commands = unwrap(encoder->finish(), "finish");
    return unwrap(device_->submit(std::move(commands)), "submit");
  }

  /// The texels of a frame cleared to \p clearColor, read back after the submission completed.
  std::vector<uint8_t> renderClearAndReadBack(const Texture& frame,
                                              std::array<double, 4> clearColor, uint32_t width,
                                              uint32_t height) {
    Buffer readback =
        unwrap(device_->createBuffer(BufferDescriptor{"readback", uint64_t{width} * height * 4u,
                                                      BufferUsage::CopyDst | BufferUsage::MapRead}),
               "createBuffer");
    const uint64_t serial = renderClear(frame, clearColor, &readback, width, height);
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << "Command buffer did not complete cleanly: " << device_->lastErrorForTest();
    return unwrap(device_->readBackBuffer(readback), "readBackBuffer");
  }

  /// The BGRA texel at (\p x, \p y) of a readback of a \p width-wide frame.
  static std::array<uint8_t, 4> texelAt(const std::vector<uint8_t>& pixels, uint32_t width,
                                        uint32_t x, uint32_t y) {
    const size_t offset = (size_t{y} * width + x) * 4u;
    if (offset + 4u > pixels.size()) {
      return {};
    }
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
  }

  std::unique_ptr<MetalDevice> device_;
  CAMetalLayer* layer_ = nil;
};

TEST_F(MetalSurfaceTest, ReportsWhatACoreAnimationLayerCanPresent) {
  const Surface surface = unwrap(device_->createSurface(surfaceDescriptor()), "createSurface");

  const SurfaceCapabilities capabilities =
      unwrap(device_->surfaceCapabilities(surface), "surfaceCapabilities");
  EXPECT_THAT(capabilities.formats, ElementsAre(TextureFormat::BGRA8Unorm))
      << "Core Animation displays a Metal layer in a fixed set of pixel formats";
  EXPECT_THAT(capabilities.presentModes, Contains(PresentMode::Fifo));
  EXPECT_THAT(capabilities.alphaModes, Contains(SurfaceAlphaMode::Opaque));
  EXPECT_TRUE(
      HasAllFlags(capabilities.usages, TextureUsage::RenderAttachment | TextureUsage::CopySrc))
      << "A frame this runtime can draw into and read back is the whole point of the surface";
}

TEST_F(MetalSurfaceTest, RefusesAPlatformObjectThatIsNotAMetalLayer) {
  NSString* notALayer = @"not a layer";
  SurfaceDescriptor descriptor = surfaceDescriptor();
  descriptor.native.display = (__bridge void*)notALayer;

  EXPECT_THAT(device_->createSurface(descriptor),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("CAMetalLayer")));
}

TEST_F(MetalSurfaceTest, RefusesAConfigurationTheLayerDoesNotSupport) {
  const Surface surface = unwrap(device_->createSurface(surfaceDescriptor()), "createSurface");

  SurfaceConfiguration otherFormat = configuration();
  otherFormat.format = TextureFormat::RGBA8Unorm;
  EXPECT_THAT(device_->configureSurface(surface, otherFormat),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("RGBA8Unorm")));

  SurfaceConfiguration mailbox = configuration();
  mailbox.presentMode = PresentMode::Mailbox;
  EXPECT_THAT(device_->configureSurface(surface, mailbox),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("Mailbox")));

  SurfaceConfiguration inheritedAlpha = configuration();
  inheritedAlpha.alphaMode = SurfaceAlphaMode::Inherit;
  EXPECT_THAT(device_->configureSurface(surface, inheritedAlpha),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("Inherit")));

  SurfaceConfiguration storage = configuration();
  storage.usage = TextureUsage::RenderAttachment | TextureUsage::StorageBinding;
  EXPECT_THAT(device_->configureSurface(surface, storage), IsGpuError(GpuErrorType::Unsupported));
}

TEST_F(MetalSurfaceTest, AcquiringBeforeConfiguringIsReported) {
  const Surface surface = unwrap(device_->createSurface(surfaceDescriptor()), "createSurface");
  EXPECT_THAT(device_->acquireCurrentTexture(surface), IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(MetalSurfaceTest, AFrameIsARenderTargetTheRuntimeCanDrawIntoAndReadBack) {
  const Surface surface = configuredSurface();

  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_EQ(frame.status, SurfaceStatus::Success);
  ASSERT_TRUE(frame.texture.isValid());

  const std::vector<uint8_t> pixels =
      renderClearAndReadBack(frame.texture, kRedClear, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(pixels.size(), size_t{kSurfaceWidth} * kSurfaceHeight * 4u)
      << "A frame is the extent the surface was configured with";
  EXPECT_THAT(texelAt(pixels, kSurfaceWidth, 0, 0), ElementsAreArray(kRedBgra));
  EXPECT_THAT(texelAt(pixels, kSurfaceWidth, kSurfaceWidth - 1, kSurfaceHeight - 1),
              ElementsAreArray(kRedBgra));
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());

  EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);
  EXPECT_THAT(device_->createTextureView(frame.texture, TextureViewDescriptor{"after"}),
              IsGpuError(GpuErrorType::InvalidHandle))
      << "The layer owns the frame once it has been handed over";
}

TEST_F(MetalSurfaceTest, RefusesAFrameWhoseExtentDoesNotMatchTheConfiguration) {
  const Surface surface = configuredSurface();

  // Moving the layer's drawable extent behind the surface's back is what a second owner of the
  // layer would do. The runtime would otherwise describe the next frame to every range check as
  // the extent it configured, while the frame itself is a different size.
  layer_.drawableSize = CGSizeMake(kSurfaceWidth / 2.0, kSurfaceHeight / 2.0);

  EXPECT_THAT(device_->acquireCurrentTexture(surface),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("configuration")))
      << "A frame that is not the configured size is refused rather than handed over mislabelled";
}

TEST_F(MetalSurfaceTest, RefusesAConfigurationWiderThanATextureMayBe) {
  const Surface surface = unwrap(device_->createSurface(surfaceDescriptor()), "createSurface");
  EXPECT_THAT(device_->configureSurface(surface, configuration(kMaxTextureDimension + 1, 64)),
              IsGpuError(GpuErrorType::LimitExceeded))
      << "Core Animation would clamp this to what it can allocate, leaving every later range "
         "check measuring frames against an extent nothing allocated";
}

TEST_F(MetalSurfaceTest, PresentsMoreFramesThanTheLayerHoldsDrawables) {
  const Surface surface = configuredSurface();

  // A layer hands out two or three drawables and reclaims one only when a presented frame has
  // been shown, so a present that never completed would starve this loop well before it ends.
  for (int frameIndex = 0; frameIndex < 8; ++frameIndex) {
    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_EQ(frame.status, SurfaceStatus::Success) << "frame " << frameIndex;
    ASSERT_TRUE(frame.texture.isValid()) << "frame " << frameIndex;

    const uint64_t serial =
        renderClear(frame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
    EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success)
        << "frame " << frameIndex;
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << "frame " << frameIndex << ": " << device_->lastErrorForTest();
  }

  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalSurfaceTest, PresentsOnceThisFramesWorkIsDoneRatherThanTheDevicesNewest) {
  const Surface surface = configuredSurface();
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(frame.texture.isValid());

  const uint64_t frameSerial =
      renderClear(frame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  ASSERT_TRUE(device_->waitForSerial(frameSerial, /*timeoutSeconds=*/30.0))
      << device_->lastErrorForTest();

  // Whatever the caller queues between drawing a frame and showing it is not the frame's work,
  // and presenting must not wait on it. Held open so that waiting on it would be visible: a
  // present that took the device's newest submission for this frame's would block until it gave
  // up, and report a frame that is finished and ready as one that never completed.
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  std::unique_ptr<CommandEncoder> unrelated =
      unwrap(device_->createCommandEncoder(), "createCommandEncoder");
  CommandBuffer unrelatedCommands = unwrap(unrelated->finish(), "finish");
  const uint64_t unrelatedSerial = unwrap(device_->submit(std::move(unrelatedCommands)), "submit");
  EXPECT_GT(unrelatedSerial, frameSerial);

  const Result<SurfaceStatus> presented = device_->presentSurface(surface);
  device_->resumeSubmissionsForTest();

  EXPECT_THAT(presented, IsOk())
      << "This frame's work completed before the present, so there was nothing to wait for";
  // Guarded rather than unwrapped: reading the value of a failed result aborts, which would end
  // the whole binary on the very failure this case exists to report.
  if (!presented.hasError()) {
    EXPECT_EQ(presented.result(), SurfaceStatus::Success);
  }
}

TEST_F(MetalSurfaceTest, AbandonsMoreFramesThanTheLayerHoldsDrawables) {
  const Surface surface = configuredSurface();

  // The same starvation check for the frame nobody shows: abandoning has to hand the drawable
  // back to the layer, not merely drop the runtime's handle to it.
  for (int frameIndex = 0; frameIndex < 8; ++frameIndex) {
    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_EQ(frame.status, SurfaceStatus::Success) << "frame " << frameIndex;
    EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk()) << "frame " << frameIndex;
  }
}

TEST_F(MetalSurfaceTest, ReconfiguringHandsOutFramesAtTheNewExtent) {
  constexpr uint32_t kResizedWidth = 128;
  constexpr uint32_t kResizedHeight = 32;

  const Surface surface = configuredSurface();
  SurfaceTexture before = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(before.texture.isValid());

  // A resize reconfigures the same surface; the frame taken under the old extent goes back.
  ASSERT_THAT(device_->configureSurface(surface, configuration(kResizedWidth, kResizedHeight)),
              IsOk());
  EXPECT_THAT(device_->createTextureView(before.texture, TextureViewDescriptor{"stale"}),
              IsGpuError(GpuErrorType::InvalidHandle));

  SurfaceTexture after = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_EQ(after.status, SurfaceStatus::Success);
  ASSERT_TRUE(after.texture.isValid());

  const std::vector<uint8_t> pixels =
      renderClearAndReadBack(after.texture, kRedClear, kResizedWidth, kResizedHeight);
  EXPECT_EQ(pixels.size(), size_t{kResizedWidth} * kResizedHeight * 4u)
      << "The layer's drawables follow the configuration rather than the surface's first extent";
  EXPECT_THAT(texelAt(pixels, kResizedWidth, kResizedWidth - 1, kResizedHeight - 1),
              ElementsAreArray(kRedBgra));
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(MetalSurfaceTest, ALayerWhoseGeometryHasMovedOnStillHandsBackAUsableFrame) {
  const Surface surface = configuredSurface();

  // Laying the layer out at a size the configuration does not cover is exactly the window resize
  // nobody has reconfigured for yet.
  layer_.bounds = CGRectMake(0.0, 0.0, kSurfaceWidth * 2.0, kSurfaceHeight * 2.0);

  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  EXPECT_EQ(frame.status, SurfaceStatus::Outdated);
  EXPECT_TRUE(frame.texture.isValid())
      << "An outdated surface still presents, so the caller chooses between drawing and resizing";
  EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk());

  ASSERT_THAT(
      device_->configureSurface(surface, configuration(kSurfaceWidth * 2, kSurfaceHeight * 2)),
      IsOk());
  SurfaceTexture resized = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  EXPECT_EQ(resized.status, SurfaceStatus::Success)
      << "Reconfiguring to the layer's geometry is the recovery from an outdated surface";
}

TEST_F(MetalSurfaceTest, ALayerThatNoLongerRendersWithThisDeviceIsLost) {
  const Surface surface = configuredSurface();
  layer_.device = nil;

  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  EXPECT_EQ(frame.status, SurfaceStatus::Lost);
  EXPECT_FALSE(frame.texture.isValid())
      << "A lost surface has no frame to hand back; only a new surface recovers";
}

TEST_F(MetalSurfaceTest, DestroyingASurfaceReturnsTheFrameItWasHolding) {
  Surface surface = configuredSurface();
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(frame.texture.isValid());

  ASSERT_THAT(device_->destroySurface(std::move(surface)), IsOk());

  // A new surface over the same layer finds every drawable available again, which it would not if
  // destruction had merely forgotten the one the destroyed surface held.
  const Surface replacement = configuredSurface();
  for (int frameIndex = 0; frameIndex < 4; ++frameIndex) {
    SurfaceTexture next =
        unwrap(device_->acquireCurrentTexture(replacement), "acquireCurrentTexture");
    ASSERT_EQ(next.status, SurfaceStatus::Success) << "frame " << frameIndex;
    EXPECT_THAT(device_->abandonCurrentTexture(replacement), IsOk()) << "frame " << frameIndex;
  }
}

}  // namespace
}  // namespace donner::gpu::metal::tests
