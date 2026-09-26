#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::geode {
namespace {

using testing::Eq;
using testing::Gt;

TEST(GeodeIdleRetirement, ReclaimsCrossThreadHandlesAndCompletedWorkWithoutAnotherFrame) {
  std::unique_ptr<GeodeDevice> context = GeodeDevice::CreateHeadless();
  ASSERT_NE(context, nullptr);
  gpu::Device& device = context->runtimeDevice();

  gpu::Texture texture = gpu::GetResultOrFail(device.createTexture(gpu::TextureDescriptor{
      "idle source", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::CopySrc}));
  gpu::Texture directTexture = gpu::GetResultOrFail(
      device.createTexture(gpu::TextureDescriptor{"directly retired source",
                                                  {4, 4},
                                                  gpu::TextureFormat::RGBA8Unorm,
                                                  gpu::TextureUsage::CopySrc}));
  gpu::Buffer buffer = gpu::GetResultOrFail(device.createBuffer(
      gpu::BufferDescriptor{"idle destination", 256 * 4, gpu::BufferUsage::CopyDst}));
  const uint32_t textureSlot = texture.slotIndex();
  const uint32_t directTextureSlot = directTexture.slotIndex();
  const uint32_t bufferSlot = buffer.slotIndex();

  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(device.createCommandEncoder());
  ASSERT_THAT(encoder->copyTextureToBuffer(gpu::TexelCopyTextureInfo{texture}, buffer,
                                           gpu::TexelCopyBufferLayout{0, 256, 4}, {4, 4}),
              gpu::IsOk());
  ASSERT_THAT(encoder->copyTextureToBuffer(gpu::TexelCopyTextureInfo{directTexture}, buffer,
                                           gpu::TexelCopyBufferLayout{0, 256, 4}, {4, 4}),
              gpu::IsOk());
  const uint64_t serial =
      gpu::GetResultOrFail(device.submit(gpu::GetResultOrFail(encoder->finish())));
  ASSERT_THAT(serial, Gt(0u));
  ASSERT_THAT(device.destroyTexture(std::move(directTexture)), gpu::IsOk());

  // The renderer has stopped submitting. Another thread drops its document, returning handles
  // to the owner instead of touching that owner's single-threaded resource tables.
  std::vector<gpu::Buffer> buffers;
  buffers.push_back(std::move(buffer));
  std::vector<gpu::BindGroup> groups;
  std::thread retire([&] {
    EXPECT_THAT(context->handleRetirement()->retire(buffers, groups), Eq(0u));
    EXPECT_TRUE(context->deferDestroyTextureBacking(std::move(texture)));
  });
  retire.join();
  ASSERT_THAT(context->retiredHandleCountsForTesting().buffers, Eq(1u));
  ASSERT_THAT(context->deferredTextureDestroyCountForTesting(), Eq(1u));

  // Completion may happen before or after the first nonblocking tick. Nothing submits or draws
  // after the copy; the owner only runs its idle maintenance hook.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    context->pollIdle();
    if (device.completedSerial() >= serial) {
      break;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_THAT(device.completedSerial(), testing::Ge(serial));
  context->pollIdle();
  EXPECT_THAT(context->retiredHandleCountsForTesting().buffers, Eq(0u));
  EXPECT_THAT(context->deferredTextureDestroyCountForTesting(), Eq(0u));

  // Reusing the buffer and both texture slots proves the pending backend objects and the
  // mailbox's serial-held texture slot were reclaimed. No later submission or teardown did it.
  const gpu::Buffer recycledBuffer = gpu::GetResultOrFail(device.createBuffer(
      gpu::BufferDescriptor{"recycled buffer", 256 * 4, gpu::BufferUsage::CopyDst}));
  const gpu::Texture recycledTexture =
      gpu::GetResultOrFail(device.createTexture(gpu::TextureDescriptor{
          "recycled texture", {4, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::CopySrc}));
  const gpu::Texture secondRecycledTexture = gpu::GetResultOrFail(
      device.createTexture(gpu::TextureDescriptor{"second recycled texture",
                                                  {4, 4},
                                                  gpu::TextureFormat::RGBA8Unorm,
                                                  gpu::TextureUsage::CopySrc}));
  EXPECT_THAT(recycledBuffer.slotIndex(), Eq(bufferSlot));
  const std::vector<uint32_t> recycledTextureSlots = {recycledTexture.slotIndex(),
                                                      secondRecycledTexture.slotIndex()};
  EXPECT_THAT(recycledTextureSlots, testing::UnorderedElementsAre(textureSlot, directTextureSlot));
}

}  // namespace
}  // namespace donner::geode
