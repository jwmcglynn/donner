/// @file
/// Physical residency of the Metal backend across frames: the allocated size Metal reports for the
/// device, sampled while a thread renders frames that each create and release their own scratch
/// resources, as a renderer's per-frame work does.

#import <Metal/Metal.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal::tests {
namespace {

using testing::AllOf;
using testing::Each;
using testing::SizeIs;

/// Width and height of each measured frame's scratch render target. At 2048x2048 RGBA8 it is
/// 16 MiB, far more than anything else a frame allocates, so a frame whose scratch is still
/// resident shows.
constexpr uint32_t kScratchExtent = 2048;
/// Width and height of the warm-up frame's render target: the narrowest RGBA8 target whose rows
/// meet the copy row alignment. Its scratch is 16 KiB, so the level read right after the warm-up
/// is close to the settled one even before Metal releases the warm-up's command buffer.
constexpr uint32_t kWarmUpExtent = kTexelRowPitchAlignment / 4u;
/// Bytes of the scratch render target, and of the readback buffer that holds a copy of it.
constexpr uint64_t kScratchTextureBytes = uint64_t{kScratchExtent} * kScratchExtent * 4u;
/// Bytes one measured frame allocates and releases: the render target and its readback buffer.
constexpr uint64_t kScratchBytesPerFrame = 2 * kScratchTextureBytes;
/// Measured frames; each would add a frame of scratch if any of it stayed resident.
constexpr int kFrames = 8;
/// How far above its level after the warm-up frame the device may be once a frame's scratch is
/// released: half of one 16 MiB scratch resource. A frame with either resource still counted
/// exceeds it, as does residue a few frames accumulate. The few MiB of bookkeeping Metal frees
/// with each command buffer stay under it, including the warm-up's own when the level read after
/// the warm-up still counts it.
constexpr uint64_t kResidueAllowanceBytes = kScratchTextureBytes / 2;
/// How long Metal may take to release a completed frame's scratch. It usually releases it within
/// about a millisecond; only a frame whose scratch is never released spends it all.
constexpr std::chrono::seconds kReleaseDeadline{5};

constexpr double kMiB = 1024.0 * 1024.0;

/// Formats a byte count in MiB, for a failure message.
/// @param bytes Bytes to format.
std::string FormatMiB(uint64_t bytes) {
  std::ostringstream stream;
  stream.precision(1);
  stream << std::fixed << static_cast<double>(bytes) / kMiB << " MiB";
  return stream.str();
}

/// The device's allocated size once a frame's scratch has left it, or when the wait for that
/// gave up.
struct SettledAllocation {
  uint64_t bytes = 0;  //!< Allocated bytes at the last read.
  /// Time from the end of the frame's wait to that read.
  std::chrono::steady_clock::duration waited{};
  bool gaveUp = false;  //!< Whether \ref kReleaseDeadline passed before the size fell far enough.

  /// Prints the read and how long it waited, for a failure message.
  /// @param settled The read. @param os Output stream.
  friend void PrintTo(const SettledAllocation& settled, std::ostream* os) {
    std::ostringstream stream;
    stream.precision(1);
    stream << std::fixed << FormatMiB(settled.bytes) << " after "
           << std::chrono::duration<double, std::milli>(settled.waited).count() << " ms";
    if (settled.gaveUp) {
      stream << ", when the wait gave up";
    }
    *os << stream.str();
  }
};

/// Matches a \ref SettledAllocation whose allocated size fell below \p ceilingBytes.
MATCHER_P(FellBelow, ceilingBytes,
          std::string(negation ? "did not fall" : "fell") + " below " + FormatMiB(ceilingBytes)) {
  return arg.bytes < ceilingBytes;
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

  /**
   * Metal's allocated size for the device once it falls below \p ceiling, or once
   * \ref kReleaseDeadline has passed without it doing so.
   *
   * The wait for a frame ends when the device publishes the frame's completion, which it does from
   * inside the command buffer's completion handler. Metal releases the command buffer, and the
   * scratch it retains, only after that handler returns, on its own completion thread, and neither
   * the device nor Metal reports when it has. Read right after the wait, the allocated size can
   * still count some or all of the frame's scratch, so it is read again until the scratch has left.
   *
   * @param ceiling Bytes the allocated size has to fall below.
   */
  SettledAllocation settledAllocation(uint64_t ceiling) const {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      const uint64_t allocated = allocatedBytes();
      const auto waited = std::chrono::steady_clock::now() - start;
      if (allocated < ceiling) {
        return SettledAllocation{allocated, waited, /*gaveUp=*/false};
      }
      if (waited >= kReleaseDeadline) {
        return SettledAllocation{allocated, waited, /*gaveUp=*/true};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  /// Renders one frame and waits for it: a scratch render target is cleared and copied into a
  /// scratch readback buffer, and the device releases both once the work completes.
  /// @param extent Width and height of the scratch render target.
  void renderFrame(uint32_t extent) {
    const Texture target = GetResultOrFail(device_->createTexture(
        TextureDescriptor{"scratch target", Extent2d{extent, extent}, TextureFormat::RGBA8Unorm,
                          TextureUsage::RenderAttachment | TextureUsage::CopySrc}));
    const TextureView view =
        GetResultOrFail(device_->createTextureView(target, TextureViewDescriptor{"scratch"}));
    const Buffer readback = GetResultOrFail(
        device_->createBuffer(BufferDescriptor{"scratch readback", uint64_t{extent} * extent * 4u,
                                               BufferUsage::CopyDst | BufferUsage::MapRead}));
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());

    RenderPassDescriptor pass;
    pass.label = "scratch clear";
    pass.colorAttachments.push_back(
        RenderPassColorAttachment{view, LoadOp::Clear, StoreOp::Store, {0.0, 0.0, 1.0, 1.0}});
    RenderPassEncoder* renderPass = GetResultOrFail(encoder->beginRenderPass(pass));
    ASSERT_NE(renderPass, nullptr);
    EXPECT_THAT(renderPass->end(), IsOk());
    EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{target}, readback,
                                             TexelCopyBufferLayout{0, extent * 4u, extent},
                                             Extent2d{extent, extent}),
                IsOk());

    const uint64_t serial = GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << "frame did not complete: " << device_->lastErrorForTest();
  }

  std::unique_ptr<MetalDevice> device_;
  id<MTLDevice> systemDevice_ = nil;
};

/// A renderer's worker is a plain thread with no autorelease pool, and such a thread drains its
/// autoreleased objects only when it exits. Each frame's scratch is released once its work
/// completes, so once a small warm-up frame has created the device's lasting state, the allocated
/// size returns to that level after every frame: an autoreleased object that still referenced a
/// frame's scratch would hold it resident until the thread exits, and the size would not return.
TEST_F(MetalWorkingSetTest, SerializedFramesOnAThreadWithoutAnAutoreleasePoolStayFlat) {
  uint64_t warmedUpBytes = 0;
  uint64_t ceilingBytes = 0;
  std::vector<SettledAllocation> afterFrame;
  std::thread worker([&] {
    renderFrame(kWarmUpExtent);
    warmedUpBytes = allocatedBytes();
    ceilingBytes = warmedUpBytes + kResidueAllowanceBytes;
    for (int frame = 0; frame < kFrames; ++frame) {
      renderFrame(kScratchExtent);
      afterFrame.push_back(settledAllocation(ceilingBytes));
      if (afterFrame.back().gaveUp) {
        break;  // This frame's scratch stayed; each later frame would spend the deadline too.
      }
    }
  });
  worker.join();

  EXPECT_THAT(afterFrame,
              AllOf(SizeIs(static_cast<size_t>(kFrames)), Each(FellBelow(ceilingBytes))))
      << "allocated after the warm-up frame: " << FormatMiB(warmedUpBytes) << ", so after each "
      << "frame the allocated size has to fall below " << FormatMiB(ceilingBytes) << " within "
      << kReleaseDeadline.count() << " s of its wait; each frame allocates and releases "
      << FormatMiB(kScratchBytesPerFrame) << " of scratch";
}

}  // namespace
}  // namespace donner::gpu::metal::tests
