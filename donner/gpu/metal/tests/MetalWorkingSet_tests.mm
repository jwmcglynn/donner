/// @file
/// Physical residency of the Metal backend across frames: the allocated size Metal reports for the
/// device, sampled while a thread renders frames that each create and release their own scratch
/// resources, as a renderer's per-frame work does.

#import <Metal/Metal.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal::tests {
namespace {

using testing::Le;

/// Width and height of each frame's scratch render target. At 2048x2048 RGBA8 it is 16 MiB, far
/// more than anything else a frame allocates, so a frame whose scratch is still resident shows.
constexpr uint32_t kScratchExtent = 2048;
/// Bytes of the scratch render target, and of the readback buffer that holds a copy of it.
constexpr uint64_t kScratchTextureBytes = uint64_t{kScratchExtent} * kScratchExtent * 4u;
/// Bytes one frame allocates and releases: the render target and its readback buffer.
constexpr uint64_t kScratchBytesPerFrame = 2 * kScratchTextureBytes;
/// Frames rendered after the first; each would add a frame of scratch if any of it stayed resident.
constexpr int kLaterFrames = 7;

constexpr double kMiB = 1024.0 * 1024.0;

/// Formats allocation samples in MiB, one per frame, for a failure message.
/// @param samples Allocated bytes after each frame.
std::string DescribeMiB(const std::vector<uint64_t>& samples) {
  std::ostringstream stream;
  stream.precision(1);
  stream << std::fixed;
  for (size_t frame = 0; frame < samples.size(); ++frame) {
    stream << (frame == 0 ? "" : ", ") << static_cast<double>(samples[frame]) / kMiB;
  }
  stream << " MiB";
  return stream.str();
}

class MetalWorkingSetTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "the Metal working-set tests");
    // Metal hands out one device object per process, so this is the device the runtime device
    // above allocates from, and its allocated size covers every resource the runtime created.
    systemDevice_ = MTLCreateSystemDefaultDevice();
    ASSERT_NE(systemDevice_, nil);
  }

  /// Bytes Metal currently has allocated for the device.
  uint64_t allocatedBytes() const { return systemDevice_.currentAllocatedSize; }

  /// Renders one frame and waits for it: a scratch render target is cleared and copied into a
  /// scratch readback buffer, and both are released once the work completes.
  void renderFrame() {
    const Texture target = GetResultOrFail(device_->createTexture(TextureDescriptor{
        "scratch target", Extent2d{kScratchExtent, kScratchExtent}, TextureFormat::RGBA8Unorm,
        TextureUsage::RenderAttachment | TextureUsage::CopySrc}));
    const TextureView view =
        GetResultOrFail(device_->createTextureView(target, TextureViewDescriptor{"scratch"}));
    const Buffer readback = GetResultOrFail(device_->createBuffer(BufferDescriptor{
        "scratch readback", kScratchTextureBytes, BufferUsage::CopyDst | BufferUsage::MapRead}));
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());

    RenderPassDescriptor pass;
    pass.label = "scratch clear";
    pass.colorAttachments.push_back(
        RenderPassColorAttachment{view, LoadOp::Clear, StoreOp::Store, {0.0, 0.0, 1.0, 1.0}});
    RenderPassEncoder* renderPass = GetResultOrFail(encoder->beginRenderPass(pass));
    ASSERT_NE(renderPass, nullptr);
    EXPECT_THAT(renderPass->end(), IsOk());
    EXPECT_THAT(
        encoder->copyTextureToBuffer(TexelCopyTextureInfo{target}, readback,
                                     TexelCopyBufferLayout{0, kScratchExtent * 4u, kScratchExtent},
                                     Extent2d{kScratchExtent, kScratchExtent}),
        IsOk());

    const uint64_t serial = GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << "frame did not complete: " << device_->lastErrorForTest();
  }

  std::unique_ptr<MetalDevice> device_;
  id<MTLDevice> systemDevice_ = nil;
};

/// A renderer's worker is a plain thread with no autorelease pool, and such a thread drains its
/// autoreleased objects only when it exits. Each frame's scratch is released as soon as its work
/// completes, so after the first frame the device's allocated size stays where it was: an
/// autoreleased object that still referenced a frame's scratch would hold it resident until the
/// thread exits, adding a frame of scratch per frame.
TEST_F(MetalWorkingSetTest, SerializedFramesOnAThreadWithoutAnAutoreleasePoolStayFlat) {
  std::vector<uint64_t> allocatedAfterFrame;
  std::thread worker([&] {
    for (int frame = 0; frame <= kLaterFrames; ++frame) {
      renderFrame();
      allocatedAfterFrame.push_back(allocatedBytes());
    }
  });
  worker.join();

  ASSERT_EQ(allocatedAfterFrame.size(), static_cast<size_t>(kLaterFrames + 1));
  const int64_t growthBytes = static_cast<int64_t>(allocatedAfterFrame.back()) -
                              static_cast<int64_t>(allocatedAfterFrame.front());
  // One frame of slack: Metal can release the latest frame's command buffer, and the scratch it
  // references, just after the wait for that frame returns.
  EXPECT_THAT(growthBytes, Le(static_cast<int64_t>(kScratchBytesPerFrame)))
      << "allocated after each frame: " << DescribeMiB(allocatedAfterFrame) << "; each frame "
      << "allocates and releases " << static_cast<double>(kScratchBytesPerFrame) / kMiB
      << " MiB of scratch";
}

}  // namespace
}  // namespace donner::gpu::metal::tests
