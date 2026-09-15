/// @file
/// Metal writeTexture destination-origin conformance: uploads a sub-rectangle at a nonzero origin
/// through donner::gpu::metal::MetalDevice and compares the whole destination byte-for-byte
/// against the shared expected image, so both the written texels and the untouched ones around
/// them are checked.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/SubRectangleUploadScene.h"

namespace donner::gpu::metal::tests {
namespace {

using gpu::tests::kSubRectUploadBytesPerRow;
using gpu::tests::kSubRectUploadExtent;
using gpu::tests::kSubRectUploadHeight;
using gpu::tests::kSubRectUploadWidth;
using gpu::tests::kSubRectUploadX;
using gpu::tests::kSubRectUploadY;
using gpu::tests::SubRectUploadBytes;
using gpu::tests::SubRectUploadBytesAt;
using gpu::tests::SubRectUploadCoversDestination;
using gpu::tests::SubRectUploadDestinationFill;
using gpu::tests::SubRectUploadDestinationFillBytes;
using gpu::tests::SubRectUploadExpectedTexel;
using gpu::tests::SubRectUploadTexel;

/// Column of a second rectangle, disjoint from the scene's own and differing on both axes so a
/// backend that mixed the two origins up would misplace texels rather than land them anyway.
constexpr uint32_t kSecondUploadX = 0;

/// Row of that second rectangle.
constexpr uint32_t kSecondUploadY = 5;

/// True if destination texel (\p x, \p y) lies inside the second rectangle.
/// @param x Column in the destination texture. @param y Row in the destination texture.
bool CoversSecondRectangle(uint32_t x, uint32_t y) {
  return x >= kSecondUploadX && x < kSecondUploadX + kSubRectUploadWidth && y >= kSecondUploadY &&
         y < kSecondUploadY + kSubRectUploadHeight;
}

/// Expected destination texel once both rectangles have been uploaded. Every uploaded texel
/// encodes its own destination, so either rectangle expects the same value there.
/// @param x Column in the destination texture. @param y Row in the destination texture.
std::array<uint8_t, 4> ExpectedTexelWithBothRectangles(uint32_t x, uint32_t y) {
  if (SubRectUploadCoversDestination(x, y) || CoversSecondRectangle(x, y)) {
    return SubRectUploadTexel(x, y);
  }
  return SubRectUploadDestinationFill();
}

/// Extent of the destination texture.
constexpr Extent2d kDestinationExtent{kSubRectUploadExtent, kSubRectUploadExtent};

/// Layout of the whole-texture fill and of the readback.
constexpr TexelCopyBufferLayout kFullLayout{0, kSubRectUploadBytesPerRow, kSubRectUploadExtent};

/// Layout of the sub-rectangle payload.
constexpr TexelCopyBufferLayout kUploadLayout{0, kSubRectUploadBytesPerRow, kSubRectUploadHeight};

class MetalSubRectangleUploadTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "Metal writeTexture destination-origin conformance");
  }

  void TearDown() override {
    if (device_) {
      device_->resumeSubmissionsForTest();
    }
  }

  /// Creates the sentinel-filled destination each case uploads into.
  Texture createFilledDestination() {
    Texture destination = GetResultOrFail(device_->createTexture(
        TextureDescriptor{"destination", kDestinationExtent, TextureFormat::RGBA8Unorm,
                          TextureUsage::CopyDst | TextureUsage::CopySrc}));
    EXPECT_THAT(device_->writeTexture(destination, SubRectUploadDestinationFillBytes(), kFullLayout,
                                      kDestinationExtent),
                IsOk());
    return destination;
  }

  /// Copies the whole destination into a fresh readback buffer, reporting the submission serial.
  /// @param destination Texture to capture. @param serial Set to the submission's serial.
  Buffer captureDestination(const Texture& destination, uint64_t& serial) {
    Buffer readback = GetResultOrFail(device_->createBuffer(
        BufferDescriptor{"readback", uint64_t{kSubRectUploadBytesPerRow} * kSubRectUploadExtent,
                         BufferUsage::CopyDst | BufferUsage::MapRead}));
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
    EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{destination}, readback,
                                             kFullLayout, kDestinationExtent),
                IsOk());
    serial = GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
    return readback;
  }

  /// Compares every destination texel against the expected image.
  /// @param pixels Readback bytes, one row per \ref kSubRectUploadBytesPerRow.
  void expectExpectedImage(const std::vector<uint8_t>& pixels) {
    for (uint32_t y = 0; y < kSubRectUploadExtent; ++y) {
      for (uint32_t x = 0; x < kSubRectUploadExtent; ++x) {
        const size_t offset = size_t{y} * kSubRectUploadBytesPerRow + size_t{x} * 4u;
        const std::array<uint8_t, 4> actual = {pixels[offset + 0], pixels[offset + 1],
                                               pixels[offset + 2], pixels[offset + 3]};
        EXPECT_THAT(actual, testing::ElementsAreArray(SubRectUploadExpectedTexel(x, y)))
            << "texel (" << x << ", " << y << ")";
      }
    }
  }

  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalSubRectangleUploadTest, WritesOnlyTheRectangleAtTheDestinationOrigin) {
  const Texture destination = createFilledDestination();

  ASSERT_THAT(device_->writeTexture(destination, SubRectUploadBytes(), kUploadLayout,
                                    Extent2d{kSubRectUploadWidth, kSubRectUploadHeight},
                                    Origin2d{kSubRectUploadX, kSubRectUploadY}),
              IsOk());

  uint64_t serial = 0;
  const Buffer readback = captureDestination(destination, serial);
  ASSERT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "Command buffer did not complete cleanly: " << device_->lastErrorForTest();
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());

  const Result<std::vector<uint8_t>> pixels = device_->readBackBuffer(readback);
  ASSERT_THAT(pixels, HasResult());
  expectExpectedImage(pixels.result());
}

// A texture the queue is still reading takes the staged upload path instead of an immediate
// replaceRegion, so the origin has to survive the staging buffer as well as the direct write.
TEST_F(MetalSubRectangleUploadTest, StagedUploadLandsAtTheSameDestinationOrigin) {
  const Texture destination = createFilledDestination();

  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  uint64_t busySerial = 0;
  const Buffer busyReadback = captureDestination(destination, busySerial);
  ASSERT_THAT(device_->completedSerial(), testing::Lt(busySerial));

  ASSERT_THAT(device_->writeTexture(destination, SubRectUploadBytes(), kUploadLayout,
                                    Extent2d{kSubRectUploadWidth, kSubRectUploadHeight},
                                    Origin2d{kSubRectUploadX, kSubRectUploadY}),
              IsOk());
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 1u);

  uint64_t serial = 0;
  const Buffer readback = captureDestination(destination, serial);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "Command buffer did not complete cleanly: " << device_->lastErrorForTest();
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());

  const Result<std::vector<uint8_t>> pixels = device_->readBackBuffer(readback);
  ASSERT_THAT(pixels, HasResult());
  expectExpectedImage(pixels.result());
}

// Queued writes are coalesced by destination, and the destination now includes the origin. Two
// same-size rectangles bound for different parts of one texture are different writes: merging
// them would silently drop one, which no pixel assertion elsewhere would attribute to
// coalescing. A repeat at the same origin must still merge, or every redundant upload would cost
// another staging slot.
TEST_F(MetalSubRectangleUploadTest, QueuedUploadsCoalesceByOriginAndNotBySizeAlone) {
  const Texture destination = createFilledDestination();

  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  uint64_t busySerial = 0;
  const Buffer busyReadback = captureDestination(destination, busySerial);
  ASSERT_THAT(device_->completedSerial(), testing::Lt(busySerial));

  const Extent2d uploadExtent{kSubRectUploadWidth, kSubRectUploadHeight};
  const Origin2d firstOrigin{kSubRectUploadX, kSubRectUploadY};
  const Origin2d secondOrigin{kSecondUploadX, kSecondUploadY};

  ASSERT_THAT(device_->writeTexture(destination, SubRectUploadBytes(), kUploadLayout, uploadExtent,
                                    firstOrigin),
              IsOk());
  ASSERT_THAT(device_->writeTexture(destination, SubRectUploadBytes(), kUploadLayout, uploadExtent,
                                    firstOrigin),
              IsOk());
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 1u)
      << "a repeated write to the same origin must still coalesce";

  ASSERT_THAT(
      device_->writeTexture(destination, SubRectUploadBytesAt(kSecondUploadX, kSecondUploadY),
                            kUploadLayout, uploadExtent, secondOrigin),
      IsOk());
  EXPECT_EQ(device_->writeStatsForTest().pendingWrites, 2u)
      << "two same-size writes to different origins are different destinations";

  uint64_t serial = 0;
  const Buffer readback = captureDestination(destination, serial);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "Command buffer did not complete cleanly: " << device_->lastErrorForTest();
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());

  const Result<std::vector<uint8_t>> pixels = device_->readBackBuffer(readback);
  ASSERT_THAT(pixels, HasResult());
  for (uint32_t y = 0; y < kSubRectUploadExtent; ++y) {
    for (uint32_t x = 0; x < kSubRectUploadExtent; ++x) {
      const size_t offset = size_t{y} * kSubRectUploadBytesPerRow + size_t{x} * 4u;
      const std::array<uint8_t, 4> actual = {
          pixels.result()[offset + 0], pixels.result()[offset + 1], pixels.result()[offset + 2],
          pixels.result()[offset + 3]};
      EXPECT_THAT(actual, testing::ElementsAreArray(ExpectedTexelWithBothRectangles(x, y)))
          << "texel (" << x << ", " << y << ")";
    }
  }
}

}  // namespace
}  // namespace donner::gpu::metal::tests
