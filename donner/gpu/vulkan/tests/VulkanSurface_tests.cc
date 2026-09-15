/// @file
/// The Vulkan presentation slice: a surface and its swapchain driven through the runtime's
/// surface hooks.
///
/// The surface is headless, which is the point. Presentation is the one part of this runtime a
/// machine with no display cannot otherwise exercise, and it is also the part whose
/// synchronization is easiest to get wrong, so the whole contract runs over
/// VK_EXT_headless_surface: the same swapchain, the same acquire and present semaphores, the same
/// image layouts, with no compositor at the end of it. A loader without that extension skips,
/// naming it.

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"
#include "donner/gpu/vulkan/VulkanLoader.h"

namespace donner::gpu::vulkan::tests {
namespace {

using testing::Contains;
using testing::ElementsAreArray;
using testing::HasSubstr;
using testing::Not;

/// Rows of a texel copy are 256-byte aligned, so the extents below are multiples of 64 texels
/// wide and read back without a padded staging step.
constexpr uint32_t kSurfaceWidth = 64;
constexpr uint32_t kSurfaceHeight = 48;

/// Opaque red, premultiplied RGBA. Every channel is 0 or 1 so the expected bytes are exact.
constexpr std::array<double, 4> kRedClear = {1.0, 0.0, 0.0, 1.0};

class VulkanSurfaceTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::CreateWithPresentationSupport();
    if (!device_) {
      // A conforming Vulkan 1.1 driver need not offer headless surfaces, so a runner that runs
      // every other Vulkan target may legitimately not present. Distinguish the two: no baseline
      // device on a required runner is a failure, a driver without the extension is a skip that
      // names what was missing.
      const char* required = std::getenv("DONNER_REQUIRE_VULKAN");
      const bool requireVulkan = required != nullptr && std::string_view(required) == "1";
      if (requireVulkan && VulkanDevice::Create() == nullptr) {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 but no Vulkan 1.1 device could be created";
      }
      GTEST_SKIP() << "This Vulkan loader or driver does not offer VK_KHR_surface with "
                      "VK_EXT_headless_surface and VK_KHR_swapchain, which presentation without a "
                      "window needs";
    }
  }

  /// Unwraps an RHI result, failing the test on error.
  template <typename T>
  T unwrap(Result<T>&& result, const char* what) {
    if (result.hasError()) {
      ADD_FAILURE() << what << " failed: " << result.error();
    }
    return std::move(result).result();
  }

  static SurfaceDescriptor headlessDescriptor() {
    SurfaceDescriptor descriptor;
    descriptor.label = "presentation";
    descriptor.native.kind = NativeSurfaceKind::Headless;
    return descriptor;
  }

  /// A configuration this surface accepts, at \p width by \p height.
  SurfaceConfiguration configuration(const Surface& surface, uint32_t width = kSurfaceWidth,
                                     uint32_t height = kSurfaceHeight) {
    const SurfaceCapabilities capabilities =
        unwrap(device_->surfaceCapabilities(surface), "surfaceCapabilities");
    EXPECT_THAT(capabilities.formats, Not(testing::IsEmpty()));
    EXPECT_THAT(capabilities.alphaModes, Not(testing::IsEmpty()));
    return SurfaceConfiguration{
        capabilities.formats.front(), TextureUsage::RenderAttachment | TextureUsage::CopySrc,
        Extent2d{width, height}, PresentMode::Fifo, capabilities.alphaModes.front()};
  }

  Surface configuredSurface(uint32_t width = kSurfaceWidth, uint32_t height = kSurfaceHeight) {
    Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");
    EXPECT_THAT(device_->configureSurface(surface, configuration(surface, width, height)), IsOk());
    return surface;
  }

  /// Clears \p frame to \p clearColor, optionally copying it into \p readback, and returns the
  /// submission serial.
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

  /// The texels of a frame cleared to \p clearColor, read back once its submission completed.
  std::vector<uint8_t> renderClearAndReadBack(const Texture& frame,
                                              std::array<double, 4> clearColor, uint32_t width,
                                              uint32_t height) {
    Buffer readback =
        unwrap(device_->createBuffer(BufferDescriptor{"readback", uint64_t{width} * height * 4u,
                                                      BufferUsage::CopyDst | BufferUsage::MapRead}),
               "createBuffer");
    const uint64_t serial = renderClear(frame, clearColor, &readback, width, height);
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << "Submission did not complete cleanly: " << device_->lastErrorForTest();
    return unwrap(device_->readBackBuffer(readback), "readBackBuffer");
  }

  /// The texel at (\p x, \p y) of a readback of a \p width-wide frame, in \p format's channel
  /// order normalised to RGBA so an expectation does not have to know which the surface chose.
  static std::array<uint8_t, 4> rgbaTexelAt(const std::vector<uint8_t>& pixels,
                                            TextureFormat format, uint32_t width, uint32_t x,
                                            uint32_t y) {
    const size_t offset = (size_t{y} * width + x) * 4u;
    if (offset + 4u > pixels.size()) {
      return {};
    }
    if (format == TextureFormat::BGRA8Unorm) {
      return {pixels[offset + 2], pixels[offset + 1], pixels[offset], pixels[offset + 3]};
    }
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanSurfaceTest, ADeviceCreatedWithoutPresentationSupportRefusesEverySurface) {
  // Swapchain support has to be requested before any surface exists, so a device that did not ask
  // for it can never present. Refusing here, where the surface is still the subject, is the whole
  // reason presentation is a separate creation entry point.
  std::unique_ptr<VulkanDevice> headless = VulkanDevice::Create();
  ASSERT_NE(headless, nullptr);
  EXPECT_FALSE(headless->supportsPresentation());

  EXPECT_THAT(headless->createSurface(headlessDescriptor()),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("presentation support")));
  EXPECT_TRUE(device_->supportsPresentation());
}

TEST_F(VulkanSurfaceTest, PointsAWindowSystemKindAtTheEmbedderPath) {
  // Making a surface from a raw window would need that window system's client headers. An
  // embedder already links one, so the refusal names the path that works rather than leaving
  // presentation looking unavailable.
  SurfaceDescriptor xlib;
  xlib.native.kind = NativeSurfaceKind::XlibWindow;
  xlib.native.display = this;
  xlib.native.window = 1;
  EXPECT_THAT(device_->createSurface(xlib),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("EmbedderSurface")));

  SurfaceDescriptor metal;
  metal.native.kind = NativeSurfaceKind::MetalLayer;
  metal.native.display = this;
  EXPECT_THAT(device_->createSurface(metal),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("MetalLayer")));
}

TEST_F(VulkanSurfaceTest, PresentsThroughASurfaceTheEmbedderCreatedAndStillOwns) {
  // Stands in for an embedder whose windowing library makes the surface: it creates one against
  // the instance the device exposes, hands over the handle, and keeps ownership. The runtime must
  // build and tear down a swapchain on it without ever destroying the surface itself.
  const VulkanDevice::NativeContextForTest native = device_->nativeContextForTest();
  ASSERT_NE(device_->nativeInstance(), nullptr);
  ASSERT_EQ(device_->nativeInstance(), native.instance);
  ASSERT_NE(native.api->vkCreateHeadlessSurfaceEXT, nullptr);

  VkInstance instance = static_cast<VkInstance>(native.instance);
  VkHeadlessSurfaceCreateInfoEXT surfaceInfo = {};
  surfaceInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
  VkSurfaceKHR embedderSurface = VK_NULL_HANDLE;
  ASSERT_EQ(
      native.api->vkCreateHeadlessSurfaceEXT(instance, &surfaceInfo, nullptr, &embedderSurface),
      VK_SUCCESS);

  uint64_t handle = 0;
  std::memcpy(&handle, &embedderSurface, sizeof(embedderSurface));

  {
    SurfaceDescriptor descriptor;
    descriptor.label = "embedder";
    descriptor.native.kind = NativeSurfaceKind::EmbedderSurface;
    descriptor.native.window = handle;

    Surface surface = unwrap(device_->createSurface(descriptor), "createSurface");
    ASSERT_THAT(device_->configureSurface(surface, configuration(surface)), IsOk());

    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_TRUE(frame.texture.isValid());
    const uint64_t serial =
        renderClear(frame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
    EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << device_->lastErrorForTest();

    ASSERT_THAT(device_->destroySurface(std::move(surface)), IsOk());
  }

  // Still the embedder's to destroy, and still valid: a second runtime surface over the same
  // handle would be impossible if the first had destroyed it.
  SurfaceDescriptor again;
  again.native.kind = NativeSurfaceKind::EmbedderSurface;
  again.native.window = handle;
  Surface reused = unwrap(device_->createSurface(again), "createSurface");
  EXPECT_THAT(device_->configureSurface(reused, configuration(reused)), IsOk())
      << "The runtime destroyed a surface it does not own";
  EXPECT_THAT(device_->destroySurface(std::move(reused)), IsOk());

  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
  native.api->vkDestroySurfaceKHR(instance, embedderSurface, nullptr);
}

TEST_F(VulkanSurfaceTest, ReportsWhatTheSurfaceCanPresent) {
  const Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");

  const SurfaceCapabilities capabilities =
      unwrap(device_->surfaceCapabilities(surface), "surfaceCapabilities");
  EXPECT_THAT(capabilities.formats, Not(testing::IsEmpty()))
      << "A surface that presents no format this runtime names could never be configured";
  EXPECT_THAT(capabilities.presentModes, Contains(PresentMode::Fifo))
      << "Queued presentation is always available in Vulkan";
  EXPECT_THAT(capabilities.alphaModes, Not(testing::IsEmpty()));
  EXPECT_TRUE(HasAllFlags(capabilities.usages, TextureUsage::RenderAttachment))
      << "A frame this runtime can draw into is the whole point of the surface";
}

TEST_F(VulkanSurfaceTest, RefusesAConfigurationOutsideItsCapabilities) {
  const Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");

  SurfaceConfiguration floatFormat = configuration(surface);
  floatFormat.format = TextureFormat::R8Unorm;
  EXPECT_THAT(device_->configureSurface(surface, floatFormat),
              IsGpuError(GpuErrorType::Unsupported));

  SurfaceConfiguration storage = configuration(surface);
  storage.usage = TextureUsage::RenderAttachment | TextureUsage::StorageBinding;
  const SurfaceCapabilities capabilities =
      unwrap(device_->surfaceCapabilities(surface), "surfaceCapabilities");
  if (!HasAllFlags(capabilities.usages, TextureUsage::StorageBinding)) {
    EXPECT_THAT(device_->configureSurface(surface, storage), IsGpuError(GpuErrorType::Unsupported));
  }
}

TEST_F(VulkanSurfaceTest, RejectsAConfigurationWiderThanATextureMayBe) {
  const Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");
  EXPECT_THAT(
      device_->configureSurface(surface, configuration(surface, kMaxTextureDimension + 1, 64)),
      IsGpuError(GpuErrorType::LimitExceeded));
}

TEST_F(VulkanSurfaceTest, AcquiringBeforeConfiguringIsReported) {
  const Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");
  EXPECT_THAT(device_->acquireCurrentTexture(surface), IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(VulkanSurfaceTest, AFrameIsARenderTargetTheRuntimeCanDrawIntoAndReadBack) {
  const Surface surface = configuredSurface();
  const TextureFormat format = configuration(surface).format;

  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_EQ(frame.status, SurfaceStatus::Success);
  ASSERT_TRUE(frame.texture.isValid());

  const std::vector<uint8_t> pixels =
      renderClearAndReadBack(frame.texture, kRedClear, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(pixels.size(), size_t{kSurfaceWidth} * kSurfaceHeight * 4u)
      << "A frame is the extent the surface was configured with";
  const std::array<uint8_t, 4> expected = {255, 0, 0, 255};
  EXPECT_THAT(rgbaTexelAt(pixels, format, kSurfaceWidth, 0, 0), ElementsAreArray(expected));
  EXPECT_THAT(rgbaTexelAt(pixels, format, kSurfaceWidth, kSurfaceWidth - 1, kSurfaceHeight - 1),
              ElementsAreArray(expected));
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());

  EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);
  EXPECT_THAT(device_->createTextureView(frame.texture, TextureViewDescriptor{"after"}),
              IsGpuError(GpuErrorType::InvalidHandle))
      << "The swapchain owns the frame once it has been handed over";
}

TEST_F(VulkanSurfaceTest, PresentsMoreFramesThanTheSwapchainHoldsImages) {
  const Surface surface = configuredSurface();

  // A swapchain hands out a small fixed number of images and takes one back only when it is
  // presented, so an acquisition semaphore that was never waited on, or a present that never
  // completed, starves this loop well before it ends. The validation layer's synchronization
  // checks watch the same frames go past.
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

TEST_F(VulkanSurfaceTest, PresentsAFrameNothingDrewInto) {
  const Surface surface = configuredSurface();

  // Nothing submits between the acquisition and the present, so the acquisition semaphore is
  // still outstanding and the present is what has to consume it. A semaphore left signalled is a
  // synchronization-validation error the next time it is waited on, which the next frame does.
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(frame.texture.isValid());
  EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);

  SurfaceTexture next = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  EXPECT_TRUE(next.texture.isValid());
  EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk());
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, KeepsItsAcquisitionRingStraightAcrossAnOutOfDateRebuild) {
  const Surface surface = configuredSurface();

  // One frame first, so the acquisition counter is not zero when the rebuild resets it. That is
  // the whole bug: an out-of-date acquisition rebuilds the swapchain mid-acquire, which replaces
  // the acquisition ring and restarts its counter, and a retry that keeps the slot it computed
  // against the destroyed ring signals one slot's semaphore while filing its fence under
  // another. With the counter already at one, those two slots differ for any ring size.
  SurfaceTexture warmup = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(warmup.texture.isValid());
  renderClear(warmup.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);

  device_->forceNextAcquireOutOfDateForTest(surface.slotIndex());
  SurfaceTexture rebuilt = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  EXPECT_EQ(rebuilt.status, SurfaceStatus::Outdated)
      << "A rebuilt swapchain still hands back a frame, with the signal to reconfigure";
  ASSERT_TRUE(rebuilt.texture.isValid());
  renderClear(rebuilt.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);

  // Deliberately not waiting for each frame: the fence filed against a ring slot is what stops
  // its semaphore being signalled again before the previous wait has run, so a fence filed under
  // the wrong slot only shows up while frames are still in flight.
  uint64_t lastSerial = 0;
  for (int frameIndex = 0; frameIndex < 12; ++frameIndex) {
    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_TRUE(frame.texture.isValid()) << "frame " << frameIndex;
    lastSerial = renderClear(frame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
    EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success)
        << "frame " << frameIndex;
  }

  EXPECT_TRUE(device_->waitForSerial(lastSerial, /*timeoutSeconds=*/30.0))
      << device_->lastErrorForTest();
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, OneSurfacesSubmissionDoesNotConsumeAnothersAcquisitionWait) {
  const Surface first = configuredSurface();
  const Surface second = configuredSurface();

  SurfaceTexture firstFrame =
      unwrap(device_->acquireCurrentTexture(first), "acquireCurrentTexture");
  SurfaceTexture secondFrame =
      unwrap(device_->acquireCurrentTexture(second), "acquireCurrentTexture");
  ASSERT_TRUE(firstFrame.texture.isValid());
  ASSERT_TRUE(secondFrame.texture.isValid());

  // Writing one surface's frame says nothing about when the other's is safe to write. A
  // submission that swept up both waits would leave the second surface's own writer carrying
  // none, and its first transition would race the presentation engine's read of that image.
  const uint64_t firstSerial =
      renderClear(firstFrame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  EXPECT_EQ(unwrap(device_->presentSurface(first), "presentSurface"), SurfaceStatus::Success);
  EXPECT_TRUE(device_->waitForSerial(firstSerial, /*timeoutSeconds=*/30.0))
      << device_->lastErrorForTest();

  const uint64_t secondSerial =
      renderClear(secondFrame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  EXPECT_EQ(unwrap(device_->presentSurface(second), "presentSurface"), SurfaceStatus::Success);
  EXPECT_TRUE(device_->waitForSerial(secondSerial, /*timeoutSeconds=*/30.0))
      << device_->lastErrorForTest();

  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, AbandonsMoreFramesThanTheSwapchainHoldsImages) {
  const Surface surface = configuredSurface();

  // Vulkan has no operation that gives an acquired image back, so discarding one is only made
  // good by replacing the swapchain. Running the discard more times than the swapchain holds
  // images is what proves that actually happens rather than the images being stranded.
  for (int frameIndex = 0; frameIndex < 8; ++frameIndex) {
    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_TRUE(frame.texture.isValid()) << "frame " << frameIndex;
    EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk()) << "frame " << frameIndex;
  }

  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, ReconfiguringHandsOutFramesAtTheNewExtent) {
  constexpr uint32_t kResizedWidth = 128;
  constexpr uint32_t kResizedHeight = 32;

  const Surface surface = configuredSurface();
  SurfaceTexture before = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(before.texture.isValid());

  ASSERT_THAT(
      device_->configureSurface(surface, configuration(surface, kResizedWidth, kResizedHeight)),
      IsOk());
  EXPECT_THAT(device_->createTextureView(before.texture, TextureViewDescriptor{"stale"}),
              IsGpuError(GpuErrorType::InvalidHandle));

  SurfaceTexture after = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_EQ(after.status, SurfaceStatus::Success);
  ASSERT_TRUE(after.texture.isValid());

  const std::vector<uint8_t> pixels =
      renderClearAndReadBack(after.texture, kRedClear, kResizedWidth, kResizedHeight);
  EXPECT_EQ(pixels.size(), size_t{kResizedWidth} * kResizedHeight * 4u)
      << "The swapchain's images follow the configuration rather than the surface's first extent";
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, DestroyingASurfaceReleasesItsSwapchain) {
  Surface surface = configuredSurface();
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(frame.texture.isValid());

  ASSERT_THAT(device_->destroySurface(std::move(surface)), IsOk());

  // A new surface finds a whole swapchain available again, which it would not if destruction had
  // left the previous one holding its images or its surface object.
  const Surface replacement = configuredSurface();
  for (int frameIndex = 0; frameIndex < 4; ++frameIndex) {
    SurfaceTexture next =
        unwrap(device_->acquireCurrentTexture(replacement), "acquireCurrentTexture");
    ASSERT_EQ(next.status, SurfaceStatus::Success) << "frame " << frameIndex;
    EXPECT_THAT(device_->abandonCurrentTexture(replacement), IsOk()) << "frame " << frameIndex;
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
