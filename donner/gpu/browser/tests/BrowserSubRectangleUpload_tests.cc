/// @file
/// Browser writeTexture destination-origin conformance: uploads a sub-rectangle at a nonzero
/// origin through donner::gpu::browser::BrowserDevice and compares the whole destination
/// byte-for-byte against the shared expected image, so both the written texels and the untouched
/// ones around them are checked.
///
/// The browser side here is \ref donner::gpu::browser::FakeBrowserBridge, which applies the
/// rectangle it is handed to its own copy of the texture. A backend that dropped the destination
/// origin therefore lands the rectangle at texel (0, 0) and fails this comparison, the same way
/// it would against a real browser.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/browser/BrowserDevice.h"
#include "donner/gpu/browser/tests/FakeBrowserBridge.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/SubRectangleUploadScene.h"

namespace donner::gpu::browser {
namespace {

using gpu::tests::ExpectSubRectUploadImageMatches;
using gpu::tests::kSubRectUploadBytesPerRow;
using gpu::tests::kSubRectUploadExtent;
using gpu::tests::kSubRectUploadHeight;
using gpu::tests::kSubRectUploadWidth;
using gpu::tests::kSubRectUploadX;
using gpu::tests::kSubRectUploadY;
using gpu::tests::SubRectUploadBytes;
using gpu::tests::SubRectUploadDestinationFillBytes;
using gpu::tests::SubRectUploadExpectedImageBytes;
using gpu::tests::SubRectUploadImageBytes;

/// Extent of the destination texture.
constexpr Extent2d kDestinationExtent{kSubRectUploadExtent, kSubRectUploadExtent};

/// Layout of the whole-texture fill.
constexpr TexelCopyBufferLayout kFullLayout{0, kSubRectUploadBytesPerRow, kSubRectUploadExtent};

/// Layout of the sub-rectangle payload.
constexpr TexelCopyBufferLayout kUploadLayout{0, kSubRectUploadBytesPerRow, kSubRectUploadHeight};

/// Identifier the bridge gives the first object a test creates, which here is the destination.
constexpr BrowserObjectId kDestinationId = 1;

/// Bytes one texel of the destination format occupies.
constexpr size_t kTexelBytes = 4;

/// Builds a device over a ready fake bridge, reporting the bridge so the test can read back what
/// the browser side holds. @param bridge Set to the bridge underneath the device.
std::unique_ptr<BrowserDevice> MakeDevice(FakeBrowserBridge*& bridge) {
  auto owned = std::make_unique<FakeBrowserBridge>();
  bridge = owned.get();

  BrowserDeviceRequest request = BrowserDeviceRequest::Begin(std::move(owned));
  EXPECT_THAT(request.state(), BrowserDeviceRequestState::Ready);
  return GetResultOrFail(std::move(request).take());
}

/// The destination's texels as a whole-texture image in the scene's row layout.
/// @param bridge Bridge holding the destination.
std::vector<uint8_t> DestinationImage(const FakeBrowserBridge& bridge) {
  const std::vector<uint8_t> texels = bridge.textureTexels(kDestinationId);
  EXPECT_THAT(texels.size(), size_t{kSubRectUploadExtent} * kSubRectUploadExtent * kTexelBytes);
  return SubRectUploadImageBytes([&](uint32_t x, uint32_t y) {
    const size_t offset = (size_t{y} * kSubRectUploadExtent + x) * kTexelBytes;
    if (offset + kTexelBytes > texels.size()) {
      return std::array<uint8_t, 4>{};
    }
    return std::array<uint8_t, 4>{texels[offset], texels[offset + 1], texels[offset + 2],
                                  texels[offset + 3]};
  });
}

TEST(BrowserSubRectangleUpload, WritesOnlyTheRectangleAtTheDestinationOrigin) {
  FakeBrowserBridge* bridge = nullptr;
  const std::unique_ptr<BrowserDevice> device = MakeDevice(bridge);
  ASSERT_THAT(device, testing::NotNull());

  const Texture destination = GetResultOrFail(device->createTexture(
      TextureDescriptor{"destination", kDestinationExtent, TextureFormat::RGBA8Unorm,
                        TextureUsage::CopyDst | TextureUsage::CopySrc}));

  ASSERT_THAT(device->writeTexture(destination, SubRectUploadDestinationFillBytes(), kFullLayout,
                                   kDestinationExtent),
              IsOk());
  ASSERT_THAT(device->writeTexture(destination, SubRectUploadBytes(), kUploadLayout,
                                   Extent2d{kSubRectUploadWidth, kSubRectUploadHeight},
                                   Origin2d{kSubRectUploadX, kSubRectUploadY}),
              IsOk());

  EXPECT_THAT(bridge->calls->back(),
              testing::HasSubstr("destination=(" + std::to_string(kSubRectUploadX) + "," +
                                 std::to_string(kSubRectUploadY) + ")"));
  ExpectSubRectUploadImageMatches(DestinationImage(*bridge), SubRectUploadExpectedImageBytes(),
                                  "browser_upload_origin");
}

}  // namespace
}  // namespace donner::gpu::browser
