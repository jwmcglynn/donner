/// @file
/// A second runtime device over one Vulkan root must be able to register the producer's image.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan {
namespace {

using testing::IsEmpty;
using testing::IsFalse;
using testing::NotNull;

TEST(VulkanTextureRegistration, ASecondRuntimeDeviceNamesTheProducerImage) {
  const std::shared_ptr<VulkanSharedRoot> root = VulkanDevice::CreateSharedRoot();
  ASSERT_THAT(root, NotNull()) << "the native Vulkan registration gate needs a Vulkan device";
  std::unique_ptr<VulkanDevice> producer = VulkanDevice::CreateOverSharedRoot(root);
  std::unique_ptr<VulkanDevice> consumer = VulkanDevice::CreateOverSharedRoot(root);
  ASSERT_THAT(producer, NotNull());
  ASSERT_THAT(consumer, NotNull());

  const Texture source = GetResultOrFail(producer->createTexture(
      TextureDescriptor{"producer",
                        {4, 4},
                        TextureFormat::RGBA8Unorm,
                        TextureUsage::Sampled | TextureUsage::CopySrc | TextureUsage::CopyDst}));
  const Result<TextureExport> exported = producer->exportTexture(source);
  ASSERT_THAT(exported, HasResult()) << "a sibling over the same VkDevice needs an image export";

  const Result<Texture> registered = consumer->registerTexture(exported.result());
  ASSERT_THAT(registered, HasResult())
      << "the consumer should alias the producer image through an independent runtime handle";
  EXPECT_THAT(consumer->ownsTextureBacking(registered.result()), IsFalse())
      << "the registration must not become a second owner of the native image";
}

TEST(VulkanTextureRegistration, ATextureFromAnotherNativeRootIsRefused) {
  const std::shared_ptr<VulkanSharedRoot> producerRoot = VulkanDevice::CreateSharedRoot();
  const std::shared_ptr<VulkanSharedRoot> foreignRoot = VulkanDevice::CreateSharedRoot();
  ASSERT_THAT(producerRoot, NotNull());
  ASSERT_THAT(foreignRoot, NotNull());
  std::unique_ptr<VulkanDevice> producer = VulkanDevice::CreateOverSharedRoot(producerRoot);
  std::unique_ptr<VulkanDevice> foreign = VulkanDevice::CreateOverSharedRoot(foreignRoot);
  ASSERT_THAT(producer, NotNull());
  ASSERT_THAT(foreign, NotNull());
  const Texture source = GetResultOrFail(producer->createTexture(
      TextureDescriptor{"foreign", {4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
  const Result<TextureExport> exported = producer->exportTexture(source);
  ASSERT_THAT(exported, HasResult());

  EXPECT_THAT(foreign->registerTexture(exported.result()),
              IsGpuError(GpuErrorType::DeviceMismatch));
}

/// The consumer's registration and in-flight records retain the native allocation even after
/// the producer runtime device and the export token are gone.
TEST(VulkanTextureRegistration, ARegistrationOutlivesItsProducerAndReadsExactPixels) {
  const std::shared_ptr<VulkanSharedRoot> root = VulkanDevice::CreateSharedRoot();
  ASSERT_THAT(root, NotNull());
  std::unique_ptr<VulkanDevice> producer = VulkanDevice::CreateOverSharedRoot(root);
  std::unique_ptr<VulkanDevice> consumer = VulkanDevice::CreateOverSharedRoot(root);
  ASSERT_THAT(producer, NotNull());
  ASSERT_THAT(consumer, NotNull());

  constexpr Extent2d kSize{4, 4};
  constexpr uint64_t kReadbackBytes = uint64_t{kTexelRowPitchAlignment} * kSize.height;
  const Texture source = GetResultOrFail(producer->createTexture(
      TextureDescriptor{"retained", kSize, TextureFormat::RGBA8Unorm,
                        TextureUsage::Sampled | TextureUsage::CopyDst | TextureUsage::CopySrc}));
  std::array<uint8_t, kSize.width * 4> expectedRow{};
  for (uint32_t x = 0; x < kSize.width; ++x) {
    expectedRow[x * 4 + 1] = 255;
    expectedRow[x * 4 + 3] = 255;
  }
  std::vector<uint8_t> uploaded(kReadbackBytes);
  for (uint32_t y = 0; y < kSize.height; ++y) {
    std::copy(expectedRow.begin(), expectedRow.end(),
              uploaded.begin() + static_cast<std::ptrdiff_t>(y * kTexelRowPitchAlignment));
  }
  constexpr TexelCopyBufferLayout kLayout{0, kTexelRowPitchAlignment, kSize.height};
  ASSERT_THAT(producer->writeTexture(source, uploaded, kLayout, kSize), IsOk());

  const Texture registered = [&] {
    const TextureExport exported = GetResultOrFail(producer->exportTexture(source));
    return GetResultOrFail(consumer->registerTexture(exported));
  }();
  producer.reset();

  const Buffer readback = GetResultOrFail(consumer->createBuffer(
      BufferDescriptor{"readback", kReadbackBytes, BufferUsage::CopyDst | BufferUsage::MapRead}));
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(consumer->createCommandEncoder());
  ASSERT_THAT(
      encoder->copyTextureToBuffer(TexelCopyTextureInfo{registered}, readback, kLayout, kSize),
      IsOk());
  const uint64_t serial = GetResultOrFail(consumer->submit(GetResultOrFail(encoder->finish())));
  ASSERT_THAT(consumer->waitForSerial(serial, 5.0), testing::IsTrue());
  const Result<std::vector<uint8_t>> pixels = consumer->readBackBuffer(readback);
  ASSERT_THAT(pixels, HasResult());
  ASSERT_EQ(pixels.result().size(), uploaded.size());
  for (uint32_t y = 0; y < kSize.height; ++y) {
    SCOPED_TRACE(testing::Message() << "row " << y);
    const std::span<const uint8_t> row(pixels.result().data() + y * kTexelRowPitchAlignment,
                                       expectedRow.size());
    EXPECT_THAT(row, testing::ElementsAreArray(expectedRow));
  }
  EXPECT_THAT(consumer->lastErrorForTest(), IsEmpty());
}

/// A producer upload and a sibling copy on separate runtime threads must observe whole images.
/// Both operations record image barriers, so the shared root orders their encodes through submit.
TEST(VulkanTextureRegistration, ConcurrentUploadAndRegisteredReadStayWhole) {
  const std::shared_ptr<VulkanSharedRoot> root = VulkanDevice::CreateSharedRoot();
  ASSERT_THAT(root, NotNull());
  std::unique_ptr<VulkanDevice> producer = VulkanDevice::CreateOverSharedRoot(root);
  ASSERT_THAT(producer, NotNull());
  constexpr Extent2d kSize{2, 2};
  constexpr uint64_t kBytes = uint64_t{kTexelRowPitchAlignment} * kSize.height;
  constexpr TexelCopyBufferLayout kLayout{0, kTexelRowPitchAlignment, kSize.height};
  constexpr std::array<uint8_t, 4> kRed{255, 0, 0, 255};
  constexpr std::array<uint8_t, 4> kGreen{0, 255, 0, 255};
  const auto imageBytes = [](const std::array<uint8_t, 4>& color) {
    std::vector<uint8_t> bytes(kBytes);
    for (uint32_t y = 0; y < kSize.height; ++y) {
      for (uint32_t x = 0; x < kSize.width; ++x) {
        std::copy(color.begin(), color.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(y * kTexelRowPitchAlignment + x * 4));
      }
    }
    return bytes;
  };
  const std::vector<uint8_t> red = imageBytes(kRed);
  const std::vector<uint8_t> green = imageBytes(kGreen);
  const Texture source = GetResultOrFail(producer->createTexture(
      TextureDescriptor{"concurrent", kSize, TextureFormat::RGBA8Unorm,
                        TextureUsage::CopySrc | TextureUsage::CopyDst | TextureUsage::Sampled}));
  ASSERT_THAT(producer->writeTexture(source, red, kLayout, kSize), IsOk());
  const TextureExport exported = GetResultOrFail(producer->exportTexture(source));

  std::promise<bool> readyPromise;
  std::future<bool> ready = readyPromise.get_future();
  std::promise<void> startPromise;
  std::future<void> start = startPromise.get_future();
  std::promise<void> writerDonePromise;
  std::future<void> writerDone = writerDonePromise.get_future();
  std::string readerError;
  constexpr int kIterations = 9;
  std::jthread reader(
      [&, exported, start = std::move(start), writerDone = std::move(writerDone)]() mutable {
        std::unique_ptr<VulkanDevice> consumer = VulkanDevice::CreateOverSharedRoot(root);
        if (!consumer) {
          readerError = "consumer could not open over the shared Vulkan root";
          readyPromise.set_value(false);
          return;
        }
        Result<Texture> registered = consumer->registerTexture(exported);
        if (registered.hasError()) {
          readerError = registered.error().message;
          readyPromise.set_value(false);
          return;
        }
        Result<Buffer> buffer = consumer->createBuffer(BufferDescriptor{
            "concurrent readback", kBytes, BufferUsage::CopyDst | BufferUsage::MapRead});
        if (buffer.hasError()) {
          readerError = buffer.error().message;
          readyPromise.set_value(false);
          return;
        }
        readyPromise.set_value(true);
        start.wait();

        const auto readOnce = [&](int iteration, bool expectGreen) {
          Result<std::unique_ptr<CommandEncoder>> encoder = consumer->createCommandEncoder();
          if (encoder.hasError()) {
            readerError = encoder.error().message;
            return false;
          }
          const Status copy = encoder.result()->copyTextureToBuffer(
              TexelCopyTextureInfo{registered.result()}, buffer.result(), kLayout, kSize);
          if (copy.hasError()) {
            readerError = copy.error().message;
            return false;
          }
          Result<CommandBuffer> commands = encoder.result()->finish();
          if (commands.hasError()) {
            readerError = commands.error().message;
            return false;
          }
          Result<uint64_t> serial = consumer->submit(std::move(commands).result());
          if (serial.hasError() || !consumer->waitForSerial(serial.result(), 0.5)) {
            readerError = std::format("registered read {} did not complete", iteration);
            return false;
          }
          Result<std::vector<uint8_t>> pixels = consumer->readBackBuffer(buffer.result());
          if (pixels.hasError() || pixels.result().size() != kBytes) {
            readerError = std::format("registered read {} returned no full bitmap", iteration);
            return false;
          }
          const auto& bytes = pixels.result();
          const std::array<uint8_t, 4> first{bytes[0], bytes[1], bytes[2], bytes[3]};
          if (first != kRed && first != kGreen) {
            readerError = std::format("registered read {} returned invalid first pixel", iteration);
            return false;
          }
          if (expectGreen && first != kGreen) {
            readerError = "the final registered read missed the producer's last green upload";
            return false;
          }
          for (uint32_t y = 0; y < kSize.height; ++y) {
            for (uint32_t x = 0; x < kSize.width; ++x) {
              const size_t offset = y * kTexelRowPitchAlignment + x * 4;
              const std::array<uint8_t, 4> pixel{bytes[offset], bytes[offset + 1],
                                                 bytes[offset + 2], bytes[offset + 3]};
              if (pixel != first) {
                readerError =
                    std::format("registered read {} mixed pixels at ({}, {})", iteration, x, y);
                return false;
              }
            }
          }
          return true;
        };

        for (int i = 0; i < kIterations; ++i) {
          if (!readOnce(i, false)) {
            return;
          }
        }
        writerDone.wait();
        static_cast<void>(readOnce(kIterations, true));
      });

  const bool readerReady = ready.get();
  std::string writerError;
  if (readerReady) {
    startPromise.set_value();
    for (int i = 0; i < kIterations; ++i) {
      const Status written =
          producer->writeTexture(source, i % 2 == 0 ? green : red, kLayout, kSize);
      if (written.hasError()) {
        writerError = std::format("upload {} failed: {}", i, written.error().message);
        break;
      }
      std::this_thread::yield();
    }
  }
  writerDonePromise.set_value();
  reader.join();
  EXPECT_THAT(readerReady, testing::IsTrue());
  EXPECT_THAT(writerError, IsEmpty());
  EXPECT_THAT(readerError, IsEmpty());
  EXPECT_THAT(producer->lastErrorForTest(), IsEmpty());
}

}  // namespace
}  // namespace donner::gpu::vulkan
