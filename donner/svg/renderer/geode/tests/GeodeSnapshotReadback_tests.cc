/// @file
/// Byte-parity tests for GPU-side snapshot unpremultiplication.
///
/// The GPU readback path (compute unpremultiply into a straight-alpha staging
/// texture, then a texture-to-buffer copy) must produce byte-identical output
/// to the CPU copy path. Each path is forced by controlling texture usage
/// flags: the GPU path requires TextureBinding, the CPU path requires CopySrc.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

namespace donner::svg {

namespace {

TEST(GeodeSnapshotDescriptorTests, PreservesKnownEmptyBufferInterface) {
  gpu::RecordingDevice device;
  geode::GeodeSnapshotReadbackPipeline pipeline(device);
  ASSERT_THAT(pipeline.valid(), testing::IsTrue());
  EXPECT_THAT(device.serialize(), testing::HasSubstr("bufferBindings=[]"));
}

constexpr uint32_t kWidth = 8;
constexpr uint32_t kHeight = 4;

/// Premultiplied RGBA test pixels covering every CPU unpremultiply branch:
/// a == 0 with nonzero RGB, a == 255, odd and even partial alpha with
/// nontrivial round-half-up behavior.
const std::array<uint8_t, kWidth * kHeight * 4>& premultipliedTestPixels() {
  static const std::array<uint8_t, kWidth * kHeight * 4> pixels = [] {
    std::array<uint8_t, kWidth * kHeight * 4> p = {};
    auto set = [&p](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
      const size_t off = (static_cast<size_t>(y) * kWidth + x) * 4u;
      p[off + 0] = r;
      p[off + 1] = g;
      p[off + 2] = b;
      p[off + 3] = a;
    };
    // Row 0: fully transparent with nonzero premultiplied RGB (must zero out).
    set(0, 0, 255, 128, 64, 0);
    set(1, 0, 17, 9, 3, 0);
    // Row 1: fully opaque (straight copy).
    set(1, 1, 200, 100, 50, 255);
    set(2, 1, 3, 240, 255, 255);
    // Row 2: odd partial alpha, round-half-up cases.
    set(2, 2, 2, 1, 1, 3);
    set(3, 2, 200, 128, 64, 128);
    set(4, 2, 7, 6, 5, 7);
    // Row 3: even partial alpha and a == 1 edge case.
    set(4, 3, 77, 33, 11, 64);
    set(5, 3, 255, 255, 255, 1);
    return p;
  }();
  return pixels;
}

/// A snapshot of a texture with the given capabilities, holding the test pixels.
/// @param device Device to allocate on. @param usage Capabilities the readback path must use.
RendererGeodeTextureSnapshot createTestSnapshot(const std::shared_ptr<geode::GeodeDevice>& device,
                                                gpu::TextureUsage usage) {
  gpu::Result<gpu::Texture> created = device->runtimeDevice().createTexture(
      gpu::TextureDescriptor{"SnapshotReadbackParity",
                             {kWidth, kHeight},
                             gpu::TextureFormat::RGBA8Unorm,
                             usage | gpu::TextureUsage::CopyDst});
  if (created.hasError()) {
    ADD_FAILURE() << "could not allocate the readback source: " << created.error();
    return {};
  }
  gpu::Texture texture = std::move(created).result();
  // Uploads are laid out on the runtime's row pitch, which is wider than one row of texels.
  std::array<uint8_t, gpu::kTexelRowPitchAlignment * kHeight> rows{};
  for (uint32_t y = 0; y < kHeight; ++y) {
    std::copy_n(premultipliedTestPixels().begin() + static_cast<size_t>(y) * kWidth * 4u,
                kWidth * 4u, rows.begin() + static_cast<size_t>(y) * gpu::kTexelRowPitchAlignment);
  }
  const gpu::Status written = device->runtimeDevice().writeTexture(
      texture, rows, {0, gpu::kTexelRowPitchAlignment, kHeight}, {kWidth, kHeight});
  if (written.hasError()) {
    ADD_FAILURE() << "could not upload the readback source: " << written.error();
    return {};
  }
  RendererGeodeTextureSnapshot snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      device, std::move(texture), Vector2i(static_cast<int>(kWidth), static_cast<int>(kHeight)),
      wgpu::TextureFormat::RGBA8Unorm, AlphaType::Premultiplied);
  if (!snapshot.isValid()) {
    ADD_FAILURE() << "the readback source was refused as a snapshot";
  }
  return snapshot;
}

class GeodeSnapshotReadbackTest : public ::testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }
};

/// The GPU unpremultiply path (TextureBinding, no CopySrc) and the CPU copy
/// path (CopySrc, no TextureBinding) must produce byte-identical straight-alpha
/// output for the same premultiplied input. The usage flags force each path:
/// the GPU path cannot run without TextureBinding, and the CPU copy cannot run
/// without CopySrc, so a mismatch between the two bitmaps cannot be hidden by
/// silent path fallback.
TEST_F(GeodeSnapshotReadbackTest, GpuAndCpuPathsAreByteIdentical) {
  auto device = sharedDevice();
  ASSERT_NE(device, nullptr);

  const Vector2i dimensions(static_cast<int>(kWidth), static_cast<int>(kHeight));
  RendererGeodeTextureSnapshot gpuSnapshot = createTestSnapshot(device, gpu::TextureUsage::Sampled);
  RendererGeodeTextureSnapshot cpuSnapshot = createTestSnapshot(device, gpu::TextureUsage::CopySrc);
  ASSERT_THAT(gpuSnapshot.isValid(), testing::IsTrue());
  ASSERT_THAT(cpuSnapshot.isValid(), testing::IsTrue());

  RendererBitmap gpuBitmap = gpuSnapshot.takeSnapshot();
  ASSERT_FALSE(gpuBitmap.empty());
  RendererBitmap cpuBitmap = cpuSnapshot.takeSnapshot();
  ASSERT_FALSE(cpuBitmap.empty());

  EXPECT_EQ(gpuBitmap.dimensions, dimensions);
  EXPECT_EQ(gpuBitmap.alphaType, AlphaType::Unpremultiplied);
  EXPECT_EQ(gpuBitmap.pixels, cpuBitmap.pixels)
      << "GPU unpremultiply output differs from the CPU reference path";
}

/// A readback that actually waits must account for the slices it ran: one recorded readback, at
/// least one slice, and a slice count that keeps growing across readbacks rather than resetting
/// or being reported once. The zero-slice cases are pinned elsewhere (a pre-cancelled snapshot
/// and a cancel observed before the first slice), so this is the other half of that contract -
/// without it a wrapper that recorded a constant zero would pass every existing assertion.
TEST_F(GeodeSnapshotReadbackTest, ACompletedReadbackAccountsForTheSlicesItRan) {
  auto device = sharedDevice();
  ASSERT_NE(device, nullptr);

  RendererGeodeTextureSnapshot snapshot = createTestSnapshot(device, gpu::TextureUsage::Sampled);
  ASSERT_THAT(snapshot.isValid(), testing::IsTrue());
  RendererGeode renderer(device);

  (void)renderer.consumeReadbackStats();  // Drop anything an earlier case in this suite left.

  ASSERT_FALSE(snapshot.takeSnapshot().empty());
  const RendererReadbackStats first = renderer.consumeReadbackStats();
  EXPECT_EQ(first.count, 1);
  EXPECT_GE(first.pollIterations, 1)
      << "a readback that waited for the GPU must report the slices it ran";

  ASSERT_FALSE(snapshot.takeSnapshot().empty());
  const RendererReadbackStats second = renderer.consumeReadbackStats();
  EXPECT_EQ(second.count, 1) << "each readback is recorded exactly once";
  EXPECT_GE(second.pollIterations, 1);
}

/// A snapshot against a lost device must return an empty bitmap promptly
/// instead of spending the full readback deadline waiting for a map that a
/// hung driver will never deliver. A fresh (non-shared) device is used so the
/// injected loss cannot poison the suite's shared device.
TEST_F(GeodeSnapshotReadbackTest, SnapshotOnLostDeviceFailsFast) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);

  RendererGeodeTextureSnapshot snapshot =
      createTestSnapshot(device, gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc);
  ASSERT_THAT(snapshot.isValid(), testing::IsTrue());

  device->markDeviceLost("test-injected loss");

  const auto start = std::chrono::steady_clock::now();
  const RendererBitmap bitmap = snapshot.takeSnapshot();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(bitmap.empty());
  // Well under the 10 second readback deadline: the map wait must fail fast
  // on the lost flag rather than waiting it out.
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

/// A device lost while the mapping is open must end the capture there. The mapped range can never
/// be filled by work that will not complete, so the capture reports nothing rather than reading
/// whatever the abandoned copy left in the buffer.
TEST_F(GeodeSnapshotReadbackTest, DeviceLostWhileTheMappingIsOpenEndsTheCapture) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);

  RendererGeodeTextureSnapshot snapshot =
      createTestSnapshot(device, gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc);
  ASSERT_THAT(snapshot.isValid(), testing::IsTrue());

  // Declaring the loss from the MapRequested phase puts the device into the lost state while this
  // capture's mapping is open, rather than before it ever started.
  device->setSnapshotReadbackHookForTesting([&](geode::GeodeDevice::SnapshotReadbackPhase phase) {
    if (phase == geode::GeodeDevice::SnapshotReadbackPhase::MapRequested) {
      device->markDeviceLost("test-injected loss while the mapping was open");
    }
  });

  const RendererBitmap bitmap = snapshot.takeSnapshot();
  device->setSnapshotReadbackHookForTesting({});

  EXPECT_TRUE(bitmap.empty())
      << "A capture whose device died mid-map must report nothing, not partial bytes";
  EXPECT_TRUE(device->isDeviceLost());
  const geode::GeodeDevice::ReadbackStats stats = device->consumeReadbackStats();
  EXPECT_TRUE(stats.deviceLost);
  EXPECT_EQ(stats.poolEntries, 0u)
      << "The readback set of a capture that died mid-map must not be pooled for reuse";
}

/// A capture registers the producer's texture in the capture context; it does not take it. The
/// producer still owns the allocation afterwards and the same snapshot reads the same bytes
/// again, which a capture that had consumed or released the source could not do. The source here
/// carries no CopySrc, so the only route that can read it is the GPU one: the compute pass, the
/// staging copy and the map all run on the capture context.
TEST_F(GeodeSnapshotReadbackTest, ACaptureBorrowsItsSourceAndLeavesItWithItsProducer) {
  auto device = sharedDevice();
  ASSERT_NE(device, nullptr);

  RendererGeodeTextureSnapshot snapshot = createTestSnapshot(device, gpu::TextureUsage::Sampled);
  ASSERT_THAT(snapshot.isValid(), testing::IsTrue());
  const gpu::Texture* source = snapshot.runtimeTexture();
  ASSERT_THAT(source, testing::NotNull());

  const RendererBitmap first = snapshot.takeSnapshot();
  ASSERT_THAT(first.pixels, testing::Not(testing::IsEmpty()))
      << "the capture must produce bytes; a source it could not name reads nothing";

  EXPECT_THAT(device->runtimeDevice().ownsTextureBacking(*source), testing::IsTrue())
      << "the capture names the producer's texture; the allocation stays the producer's";

  const RendererBitmap second = snapshot.takeSnapshot();
  ASSERT_THAT(second.pixels, testing::Not(testing::IsEmpty()))
      << "a source the first capture had consumed or released could not be read again";
  EXPECT_THAT(second.pixels, testing::ElementsAreArray(first.pixels))
      << "a source retained across the capture reads the same bytes the next time";
  EXPECT_THAT(second.dimensions, testing::Eq(first.dimensions));
  EXPECT_THAT(second.alphaType, testing::Eq(first.alphaType));
}

/// A texture belongs to the runtime device that allocated it: a handle is a slot plus a
/// generation, and both mean something else on another device. Admission is where that is
/// caught, so a snapshot refuses to name one device's texture against another's rather than
/// carry a handle a capture would later resolve against the wrong table.
TEST_F(GeodeSnapshotReadbackTest, AdoptingATextureOfAnotherDeviceIsRefused) {
  std::shared_ptr<geode::GeodeDevice> producer(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(producer, nullptr);
  auto consumer = sharedDevice();
  ASSERT_NE(consumer, nullptr);

  gpu::Texture ownedElsewhere =
      gpu::GetResultOrFail(producer->runtimeDevice().createTexture(gpu::TextureDescriptor{
          "OwnedByAnotherDevice",
          {kWidth, kHeight},
          gpu::TextureFormat::RGBA8Unorm,
          gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc | gpu::TextureUsage::CopyDst}));
  // Identity only, so the assertion below reads the producer's record rather than a handle the
  // refused admission may or may not have left alone.
  const gpu::Texture probe = gpu::Texture::CreateForBackend(
      ownedElsewhere.slotIndex(), ownedElsewhere.generation(), ownedElsewhere.deviceId());

  const RendererGeodeTextureSnapshot foreign = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      consumer, std::move(ownedElsewhere),
      Vector2i(static_cast<int>(kWidth), static_cast<int>(kHeight)),
      wgpu::TextureFormat::RGBA8Unorm, AlphaType::Premultiplied);
  EXPECT_THAT(foreign.isValid(), testing::IsFalse())
      << "a texture of another device must not be admitted as a snapshot of this one";
  EXPECT_THAT(producer->runtimeDevice().ownsTextureBacking(probe), testing::IsTrue())
      << "a refused admission leaves the texture with the device that allocated it";
}

/// RendererGeode surfaces the device-lost condition of its backing device.
TEST_F(GeodeSnapshotReadbackTest, RendererGeodeReportsDeviceLost) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);

  RendererGeode renderer(device);
  EXPECT_FALSE(renderer.deviceLost());
  device->markDeviceLost("test-injected loss");
  EXPECT_TRUE(renderer.deviceLost());
  // Renderer teardown after the declared loss must not block; the test
  // completing at all (under the suite timeout) pins that.
}

/// The renderer's readback stats are the only channel a frame that produced
/// nothing has left, so they must carry the device-lost outcome and the
/// attribution of the bounded wait that declared it, translated into the
/// backend-neutral vocabulary.
TEST_F(GeodeSnapshotReadbackTest, ReadbackStatsCarryWaitTimeoutAttribution) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);

  RendererGeode renderer(device);
  const RendererReadbackStats healthy = renderer.consumeReadbackStats();
  EXPECT_FALSE(healthy.deviceLost);
  EXPECT_EQ(healthy.timedOutWaitSite, GpuWaitTimeoutSite::None);
  EXPECT_EQ(healthy.timedOutWaitMs, 0);

  device->markDeviceLostAfterWaitTimeout(geode::GpuWaitSite::ReadbackMap,
                                         geode::kReadbackMapTimeout,
                                         "test-injected readback map stall");

  const RendererReadbackStats afterTimeout = renderer.consumeReadbackStats();
  EXPECT_TRUE(afterTimeout.deviceLost);
  EXPECT_EQ(afterTimeout.timedOutWaitSite, GpuWaitTimeoutSite::ReadbackMap);
  EXPECT_EQ(afterTimeout.timedOutWaitMs, static_cast<int>(geode::kReadbackMapTimeout.count()));

  const RendererReadbackStats laterFrame = renderer.consumeReadbackStats();
  EXPECT_TRUE(laterFrame.deviceLost);
  EXPECT_EQ(laterFrame.timedOutWaitSite, GpuWaitTimeoutSite::ReadbackMap);
}

/// A queue-drain stall must be reported as a different wait than a readback
/// map stall. The two have different budgets and different callers, so a
/// report that collapsed them would point at the wrong subsystem.
TEST_F(GeodeSnapshotReadbackTest, QueueIdleTimeoutIsReportedAsItsOwnWaitSite) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);

  RendererGeode renderer(device);
  device->markDeviceLostAfterWaitTimeout(geode::GpuWaitSite::QueueIdle,
                                         geode::kDefaultGpuWaitTimeout,
                                         "test-injected queue drain stall");

  const RendererReadbackStats stats = renderer.consumeReadbackStats();
  EXPECT_TRUE(stats.deviceLost);
  EXPECT_EQ(stats.timedOutWaitSite, GpuWaitTimeoutSite::QueueIdle);
  EXPECT_EQ(stats.timedOutWaitMs, static_cast<int>(geode::kDefaultGpuWaitTimeout.count()));
}

/// Memory another context still holds after its producer released it is resident but no longer
/// the producer's allocation. The readback statistics report those bytes, so a working set
/// measured through them does not lose the memory, and stop reporting them once the last holder
/// lets go.
TEST_F(GeodeSnapshotReadbackTest, ReadbackStatsReportBytesHeldAfterTheProducerReleasedThem) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_NE(device, nullptr);
  RendererGeode renderer(device);
  gpu::Device& runtime = device->runtimeDevice();

  gpu::Texture texture = gpu::GetResultOrFail(runtime.createTexture(
      gpu::TextureDescriptor{"heldElsewhere",
                             {16, 8},
                             gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc}));
  std::optional<gpu::TextureExport> exported = gpu::GetResultOrFail(runtime.exportTexture(texture));
  EXPECT_EQ(renderer.consumeReadbackStats().sharedTextureTailBytes, 0u)
      << "the producer still holds the texture";

  ASSERT_THAT(runtime.destroyTexture(std::move(texture)), gpu::IsOk());
  EXPECT_EQ(renderer.consumeReadbackStats().sharedTextureTailBytes, 16u * 8u * 4u);

  exported.reset();
  EXPECT_EQ(renderer.consumeReadbackStats().sharedTextureTailBytes, 0u);
}

}  // namespace
}  // namespace donner::svg
