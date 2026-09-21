/// @file
/// Contract coverage for the shared runtime bitmap uploader.
///
/// The uploader has no surface of its own, so its refusals are exercised directly rather than
/// through a panel or a texture cache. What matters to its callers is that an upload it cannot
/// complete costs nothing: no allocation, no write, and no snapshot handed back.

#include "donner/editor/RuntimeBitmapUpload.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::editor {
namespace {

const Vector2i kPayloadDimensions = Vector2i(3, 2);
const Vector2i kAllocationDimensions = Vector2i(4, 2);
constexpr std::size_t kPayloadRowBytes = 12u;

std::shared_ptr<geode::GeodeDevice> SharedGeodeDevice() {
  static const std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  return device;
}

/// Tightly packed RGBA payload matching \ref kPayloadDimensions.
std::vector<uint8_t> PayloadPixels() {
  std::vector<uint8_t> pixels(kPayloadRowBytes * static_cast<std::size_t>(kPayloadDimensions.y),
                              0u);
  for (std::size_t index = 0; index < pixels.size(); index += 4u) {
    pixels[index] = static_cast<uint8_t>(20u + index);
    pixels[index + 1u] = 20u;
    pixels[index + 2u] = 10u;
    pixels[index + 3u] = 0xFFu;
  }
  return pixels;
}

TEST(RuntimeBitmapUploadTest, UploadsAPayloadIntoItsOwnAllocation) {
  const std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_THAT(device, testing::NotNull());

  const std::shared_ptr<svg::RendererGeodeTextureSnapshot> first =
      UploadRuntimeBitmap(device, PayloadPixels(), kPayloadDimensions, kPayloadRowBytes,
                          svg::AlphaType::Unpremultiplied, kAllocationDimensions);
  ASSERT_THAT(first, testing::NotNull());
  EXPECT_THAT(first->dimensions(), testing::Eq(kPayloadDimensions));
  EXPECT_THAT(first->allocationDimensions(), testing::Eq(kAllocationDimensions));
  ASSERT_THAT(first->runtimeTexture(), testing::NotNull());
  EXPECT_THAT(first->runtimeTexture()->deviceId(), testing::Eq(device->runtimeDevice().deviceId()));

  const std::shared_ptr<svg::RendererGeodeTextureSnapshot> second =
      UploadRuntimeBitmap(device, PayloadPixels(), kPayloadDimensions, kPayloadRowBytes,
                          svg::AlphaType::Unpremultiplied, kAllocationDimensions);
  ASSERT_THAT(second, testing::NotNull());
  EXPECT_THAT(second.get(), testing::Ne(first.get()))
      << "Each upload must own its allocation, so a caller presenting an earlier one is never "
         "written through.";
}

TEST(RuntimeBitmapUploadTest, RefusesAnAllocationOrStorageThatCannotHoldThePayload) {
  const std::shared_ptr<geode::GeodeDevice> device = SharedGeodeDevice();
  ASSERT_THAT(device, testing::NotNull());
  const std::vector<uint8_t> pixels = PayloadPixels();
  const std::uint64_t createsBefore = device->lifetimeTextureCreates();

  EXPECT_THAT(UploadRuntimeBitmap(device, pixels, kPayloadDimensions, kPayloadRowBytes,
                                  svg::AlphaType::Unpremultiplied, Vector2i(2, 2)),
              testing::IsNull())
      << "An allocation narrower than the payload cannot hold it.";
  EXPECT_THAT(UploadRuntimeBitmap(device, pixels, Vector2i(0, 2), kPayloadRowBytes,
                                  svg::AlphaType::Unpremultiplied, kAllocationDimensions),
              testing::IsNull())
      << "A zero-area payload extent has nothing to upload.";
  EXPECT_THAT(UploadRuntimeBitmap(device, pixels, kPayloadDimensions, /*rowBytes=*/8u,
                                  svg::AlphaType::Unpremultiplied, kAllocationDimensions),
              testing::IsNull())
      << "A stride shorter than one payload row would read past the source storage.";
  EXPECT_THAT(UploadRuntimeBitmap(device, std::vector<uint8_t>(pixels.begin(), pixels.end() - 1),
                                  kPayloadDimensions, kPayloadRowBytes,
                                  svg::AlphaType::Unpremultiplied, kAllocationDimensions),
              testing::IsNull())
      << "Storage one byte short of the declared rows would read past the source storage.";
  EXPECT_THAT(device->lifetimeTextureCreates(), testing::Eq(createsBefore))
      << "Storage and extent are validated before any backing is allocated.";
}

TEST(RuntimeBitmapUploadTest, RefusesAnAbsentDevice) {
  EXPECT_THAT(UploadRuntimeBitmap(nullptr, PayloadPixels(), kPayloadDimensions, kPayloadRowBytes,
                                  svg::AlphaType::Unpremultiplied, kAllocationDimensions),
              testing::IsNull());
}

}  // namespace
}  // namespace donner::editor
