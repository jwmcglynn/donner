#include "donner/svg/renderer/geode/GeoEncoder.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/svg/renderer/geode/GeodeBufferPool.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePathCacheComponent.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::geode {

namespace {

constexpr uint32_t kSize = 64;
constexpr wgpu::TextureFormat kFormat = wgpu::TextureFormat::RGBA8Unorm;
constexpr uint32_t kBytesPerRow = 256;  // Padded from kSize*4 = 256.

using svg::test::FormatRgba;
using svg::test::Near;
using svg::test::Rgba;
using svg::test::RgbaEq;

class ProbeGeometryAdmission final : public GeometryAdmission {
public:
  bool admitGeometry(const EncodedPath& /*encoded*/, std::size_t logicalDraws) override {
    admittedDraws += logicalDraws;
    return allow;
  }

  bool canEncodeGeometry() const override { return encodeOpen; }

  void releaseGeometry(const EncodedPath& /*encoded*/, std::size_t logicalDraws) override {
    releasedDraws += logicalDraws;
  }

  bool allow = true;
  bool encodeOpen = true;
  std::size_t admittedDraws = 0;
  std::size_t releasedDraws = 0;
};

/// Test fixture: shares a process-wide device and creates per-test render
/// targets + readback buffer.
///
/// Sharing the device avoids the Mesa llvmpipe / Intel ANV driver hang caused
/// by accumulating many WebGPU device creations in a single process.
class GeoEncoderTest : public ::testing::Test {
protected:
  /// Returns a process-wide shared GeodeDevice (created once, destroyed at exit).
  static std::shared_ptr<GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<GeodeDevice>(GeodeDevice::CreateHeadless());
    }();
    return device;
  }

  void SetUp() override {
    device_ = sharedDevice();
    ASSERT_NE(device_, nullptr);

    // The device-owned shared pipelines (the shared headless device's target format is
    // kFormat). Pipeline construction lives on GeodeDevice per the ownership rule.
    pipeline_ = &device_->pipeline();
    gradientPipeline_ = &device_->gradientPipeline();
    imagePipeline_ = &device_->imagePipeline();

    wgpu::TextureDescriptor td = {};
    td.label = wgpuLabel("TestTarget");
    td.size = {kSize, kSize, 1};
    td.format = kFormat;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::TextureBinding;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.dimension = wgpu::TextureDimension::_2D;
    target_ = device_->device().createTexture(td);
    ASSERT_TRUE(static_cast<bool>(target_));

    wgpu::BufferDescriptor bd = {};
    bd.label = wgpuLabel("TestReadback");
    bd.size = kBytesPerRow * kSize;
    bd.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    readback_ = device_->device().createBuffer(bd);
    ASSERT_TRUE(static_cast<bool>(readback_));
  }

  /// Read back the rendered texture into a flat RGBA byte array (row-major,
  /// no padding - `kSize * kSize * 4` bytes).
  std::vector<uint8_t> readback() {
    // Copy texture → readback buffer.
    wgpu::CommandEncoder enc = device_->device().createCommandEncoder();

    wgpu::TexelCopyTextureInfo src = {};
    src.texture = target_;
    src.mipLevel = 0;
    src.origin = {0, 0, 0};

    wgpu::TexelCopyBufferInfo dst = {};
    dst.buffer = readback_;
    dst.layout.bytesPerRow = kBytesPerRow;
    dst.layout.rowsPerImage = kSize;

    wgpu::Extent3D copySize = {kSize, kSize, 1};
    enc.copyTextureToBuffer(src, dst, copySize);

    wgpu::CommandBuffer cmd = enc.finish();
    device_->queue().submit(1, &cmd);

    // Map readback buffer. wgpu-native's `mapAsync` only exposes the
    // callback-info form; plumb the done flag through `userdata1` and poll
    // non-blocking under a bounded wait, which drains pending callbacks
    // without risking an unbounded block inside a hung driver.
    struct MapState {
      std::atomic<bool> done = false;
      std::atomic<bool> ok = false;
    };
    auto mapState = std::make_shared<MapState>();
    wgpu::BufferMapCallbackInfo mapCb{wgpu::Default};
    mapCb.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                        void* /*userdata2*/) {
      const std::shared_ptr<MapState> state = takeWgpuCallbackState<MapState>(userdata1);
      state->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
      state->done.store(true, std::memory_order_release);
    };
    mapCb.userdata1 = retainWgpuCallbackState(mapState);
    mapCb.userdata2 = nullptr;
    readback_.mapAsync(wgpu::MapMode::Read, 0, kBytesPerRow * kSize, mapCb);
    const GpuWaitResult waitResult = BoundedGpuWait(
        [&] {
          device_->device().poll(false, nullptr);
          return mapState->done.load(std::memory_order_acquire);
        },
        kDefaultGpuWaitTimeout);
    EXPECT_EQ(waitResult, GpuWaitResult::Complete) << "buffer map wait timed out";
    EXPECT_TRUE(mapState->ok.load(std::memory_order_relaxed)) << "buffer map failed";

    const uint8_t* mapped =
        static_cast<const uint8_t*>(readback_.getConstMappedRange(0, kBytesPerRow * kSize));

    // Strip the row padding (256 bytes per row → 256 bytes per row, but the
    // visible part is kSize * 4 = 256 bytes for kSize=64, so no stripping
    // needed for our test size). Be defensive in case kSize ever changes.
    std::vector<uint8_t> pixels(kSize * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y) {
      std::copy_n(mapped + y * kBytesPerRow, kSize * 4, pixels.data() + y * kSize * 4);
    }
    readback_.unmap();
    return pixels;
  }

  /// Get the RGBA value at pixel (x, y).
  static std::array<uint8_t, 4> pixelAt(const std::vector<uint8_t>& pixels, uint32_t x,
                                        uint32_t y) {
    const size_t off = (y * kSize + x) * 4;
    return {pixels[off], pixels[off + 1], pixels[off + 2], pixels[off + 3]};
  }

  std::shared_ptr<GeodeDevice> device_;
  GeodePipeline* pipeline_ = nullptr;
  GeodeGradientPipeline* gradientPipeline_ = nullptr;
  GeodeImagePipeline* imagePipeline_ = nullptr;
  wgpu::Texture target_;
  wgpu::Buffer readback_;
};

// ----------------------------------------------------------------------------

/// Clear the direct render target and read it back without a resolve attachment.
TEST_F(GeoEncoderTest, ClearWritesDirectTarget) {
  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 128, 255, 255));  // Half-saturated blue.
  encoder.finish();

  auto pixels = readback();
  auto pixel = pixelAt(pixels, 32, 32);
  EXPECT_THAT(pixel, RgbaEq(0, 128, 255, 255));
}

/// Fill an axis-aligned rectangle and verify a center pixel is the fill color.
TEST_F(GeoEncoderTest, FillRect) {
  Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));  // Black.
  encoder.fillPath(path, css::RGBA(255, 0, 0, 255), FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Center should be red.
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_THAT(center, RgbaEq(255, 0, 0, 255)) << "Center should be red";

  // Top-left corner should be black (outside the rect).
  auto corner = pixelAt(pixels, 4, 4);
  EXPECT_THAT(corner, RgbaEq(0, 0, 0, 255)) << "Corner should be clear black";
}

TEST_F(GeoEncoderTest, RejectedPatternGeometryAllocatesNoPatternGpuState) {
  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  const EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());
  ProbeGeometryAdmission admission;
  admission.allow = false;

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.setGeometryAdmission(&admission);
  GeoEncoder::PatternPaint paint;
  paint.tile = target_;
  paint.tileSize = Vector2d(8.0, 8.0);
  encoder.fillPathPattern(path, FillRule::NonZero, paint, &encoded);

  EXPECT_EQ(admission.admittedDraws, 1u);
  EXPECT_EQ(encoder.patternGpuPreparationsForTesting(), 0u)
      << "Pattern sampler/view creation must follow successful geometry admission.";
  encoder.finish();
}

TEST_F(GeoEncoderTest, DegenerateResidentRadialDoesNotConsumeGeometryAdmission) {
  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  const EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());
  ProbeGeometryAdmission admission;
  RadialGradientParams::Stop stop;
  RadialGradientParams params;
  params.radius = 0.0;
  params.stops = std::span<const RadialGradientParams::Stop>(&stop, 1u);
  GeodeResidentGradientSlot slot;

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.setGeometryAdmission(&admission);
  encoder.fillPathRadialGradientResident(slot, encoded, params, FillRule::NonZero, 1u);
  EXPECT_EQ(admission.admittedDraws, 0u);
  encoder.finish();
}

TEST_F(GeoEncoderTest, PreparedSceneAdmissionCanBeRefundedBeforeSingletonFallback) {
  const Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();
  const EncodedPath encoded = GeodePathEncoder::encode(path, FillRule::NonZero);
  ASSERT_FALSE(encoded.empty());
  ProbeGeometryAdmission admission;

  auto geometry = std::make_shared<GeodeResidentSlab>(device_->deviceId());
  auto records = std::make_shared<GeodeRecordSlab>(device_->deviceId());
  GeodeResidentSlot slot;
  slot.slab = geometry;
  slot.recordSlab = records;
  ASSERT_TRUE(records->allocateSlot(*device_, slot.recordSlot));

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.setGeometryAdmission(&admission);
  GeoEncoder::SceneRecordState recordState;
  ASSERT_TRUE(encoder.ensureResidentSceneRecord(
      slot, encoded, GeoEncoder::ScenePaint{css::RGBA(255, 0, 0, 255)}, FillRule::NonZero,
      Transform2d(), nullptr, nullptr, &recordState));
  ASSERT_EQ(encoder.pendingSceneAdmissionsForTesting(), 1u);
  ASSERT_EQ(admission.admittedDraws, 1u);

  encoder.releasePreparedSceneAdmission(encoded);
  EXPECT_EQ(encoder.pendingSceneAdmissionsForTesting(), 0u);
  EXPECT_EQ(admission.releasedDraws, 1u);
  encoder.finish();
}

TEST_F(GeoEncoderTest, TinyUniformScaleStillRasterizesHalfPixelHalo) {
  const Path path =
      PathBuilder().addRect(Box2d({264062.5, 264062.5}, {735937.5, 735937.5})).build();

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 0));
  encoder.setTransform(Transform2d::Scale(0.000064));
  encoder.fillPath(path, css::RGBA(255, 0, 0, 255), FillRule::NonZero);
  encoder.finish();

  const auto pixels = readback();
  const auto halo = pixelAt(pixels, 16, 32);
  EXPECT_GT(halo[0], 0u);
  EXPECT_GT(halo[3], 0u);
  EXPECT_THAT(pixelAt(pixels, 15, 32), RgbaEq(0, 0, 0, 0));
}

TEST_F(GeoEncoderTest, IllConditionedShearStillRasterizesHalfPixelHalo) {
  constexpr double kPathYMin = 65500.0;
  constexpr double kPathYMax = 68500.0;
  constexpr double kDeviceXMinOverScale = -2000.0;
  constexpr double kDeviceXMaxOverScale = 2000.0;
  const Path path = PathBuilder()
                        .moveTo(Vector2d(-kPathYMin + kDeviceXMinOverScale, kPathYMin))
                        .lineTo(Vector2d(-kPathYMin + kDeviceXMaxOverScale, kPathYMin))
                        .lineTo(Vector2d(-kPathYMax + kDeviceXMaxOverScale, kPathYMax))
                        .lineTo(Vector2d(-kPathYMax + kDeviceXMinOverScale, kPathYMax))
                        .closePath()
                        .build();

  // The two device-space axes are nearly collinear: their determinant ratio
  // is below the miter path's conditioning threshold. The large path maps to
  // a visible strip from y=32.75 to y=34.25 that crosses the full viewport.
  Transform2d transform(Transform2d::uninitialized);
  transform.data[0] = 1024.0;
  transform.data[1] = 0.0;
  transform.data[2] = 1024.0;
  transform.data[3] = 0.0005;
  transform.data[4] = 0.0;
  transform.data[5] = 0.0;

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 0));
  encoder.setTransform(transform);
  encoder.fillPath(path, css::RGBA(255, 0, 0, 255), FillRule::NonZero);
  encoder.finish();

  const auto pixels = readback();
  EXPECT_GT(pixelAt(pixels, 32, 32)[3], 0u);
  EXPECT_GT(pixelAt(pixels, 32, 33)[3], 0u);
  EXPECT_GT(pixelAt(pixels, 32, 34)[3], 0u);
  EXPECT_THAT(pixelAt(pixels, 32, 31), RgbaEq(0, 0, 0, 0));
  EXPECT_THAT(pixelAt(pixels, 32, 35), RgbaEq(0, 0, 0, 0));
}

/// Record-slab slots use GLOBAL indices across chunks while byte offsets are
/// per-buffer. After the slab grows, a freed slot from a later chunk must
/// resolve back to its exact original buffer and buffer-relative offset;
/// resolving the global index as a raw byte offset would alias another live
/// slot's storage and two entities would share one record.
/// entt removes a component mid-pool by move-assigning the LAST component
/// over the removed slot (swap-and-pop). The move must transfer the record
/// slot and its owning slab: if the moved-from component kept them, its
/// destructor would free the SURVIVOR's record slot back to the slab, and a
/// later allocation would hand the same storage to another entity while the
/// survivor's cached bind group still binds it.
TEST_F(GeoEncoderTest, ResidentSlotMoveTransfersRecordSlot) {
  auto slab = std::make_shared<geode::GeodeRecordSlab>(device_->deviceId());

  geode::GeodeResidentSlot survivor;
  survivor.recordSlab = slab;
  ASSERT_TRUE(slab->allocateSlot(*device_, survivor.recordSlot));
  const geode::GeodeRecordSlab::Slot survivorSlot = survivor.recordSlot;

  {
    // Swap-and-pop shape: the survivor is move-assigned into the removed
    // component's storage; the moved-from shell is then destroyed.
    geode::GeodeResidentSlot removedStorage;
    removedStorage = std::move(survivor);
    ASSERT_TRUE(removedStorage.recordSlot.buffer);
    EXPECT_EQ(removedStorage.recordSlot.index, survivorSlot.index);
    // survivor (the moved-from shell) is destroyed at scope exit of the
    // ORIGINAL object in real usage; simulate by letting it destruct via a
    // fresh scope below. Here, verify the shell no longer owns the slot.
    EXPECT_FALSE(survivor.recordSlot.buffer)
        << "The moved-from slot must not retain the record slot";
    // Destroy the shell explicitly (what entt's pop does to the tail).
    { geode::GeodeResidentSlot shell = std::move(survivor); }

    // If the shell's destructor freed the survivor's slot, the next
    // allocation after a frame boundary would return it. It must not.
    slab->beginFrame(1);
    geode::GeodeRecordSlab::Slot next;
    ASSERT_TRUE(slab->allocateSlot(*device_, next));
    EXPECT_FALSE(next.buffer == survivorSlot.buffer && next.offset == survivorSlot.offset)
        << "The survivor's record slot leaked back to the slab through the moved-from shell";
  }
}

TEST_F(GeoEncoderTest, RecordSlabFreeListSurvivesChunkGrowth) {
  geode::GeodeRecordSlab slab(device_->deviceId());

  // Fill past the first chunk so a second chunk exists.
  std::vector<geode::GeodeRecordSlab::Slot> slots;
  const size_t kCount = 1100;  // First chunk holds 1024 records.
  slots.reserve(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    geode::GeodeRecordSlab::Slot slot;
    ASSERT_TRUE(slab.allocateSlot(*device_, slot)) << "allocation " << i;
    ASSERT_TRUE(slot.buffer);
    slots.push_back(slot);
  }
  ASSERT_NE(slots[0].buffer, slots.back().buffer) << "Fixture must span two chunks";

  // Free one slot in each chunk; frees merge at the next frame boundary.
  const geode::GeodeRecordSlab::Slot freedEarly = slots[10];
  const geode::GeodeRecordSlab::Slot freedLate = slots[1090];
  slab.freeSlot(freedEarly);
  slab.freeSlot(freedLate);
  slab.beginFrame(1);

  // Reuse resolves each index to its ORIGINAL buffer and offset.
  geode::GeodeRecordSlab::Slot reusedA;
  geode::GeodeRecordSlab::Slot reusedB;
  ASSERT_TRUE(slab.allocateSlot(*device_, reusedA));
  ASSERT_TRUE(slab.allocateSlot(*device_, reusedB));
  const auto matches = [](const geode::GeodeRecordSlab::Slot& a,
                          const geode::GeodeRecordSlab::Slot& b) {
    return a.buffer == b.buffer && a.offset == b.offset && a.index == b.index;
  };
  EXPECT_TRUE(matches(reusedA, freedEarly) || matches(reusedA, freedLate));
  EXPECT_TRUE(matches(reusedB, freedEarly) || matches(reusedB, freedLate));
  EXPECT_FALSE(matches(reusedA, reusedB));

  // No reused slot may alias a still-live slot's storage.
  for (const auto& live : slots) {
    if (matches(live, freedEarly) || matches(live, freedLate)) {
      continue;
    }
    EXPECT_FALSE(live.buffer == reusedA.buffer && live.offset == reusedA.offset);
    EXPECT_FALSE(live.buffer == reusedB.buffer && live.offset == reusedB.offset);
  }
}

TEST_F(GeoEncoderTest, ResidentSlabRejectsGrowthPastExactDocumentBudget) {
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();
  constexpr uint64_t kInitialBytes = 1u << 20;
  budget->setLimitsForTesting({.cacheBytes = 64u << 20, .residentBytes = kInitialBytes});

  {
    GeodeResidentSlab slab(device_->deviceId(), budget);
    GeodeResidentSlab::Allocation exact;
    ASSERT_TRUE(slab.allocate(*device_, kInitialBytes, /*alignment=*/256u, exact));
    EXPECT_EQ(budget->residentBytes(), kInitialBytes);

    GeodeResidentSlab::Allocation capPlusOne;
    EXPECT_FALSE(slab.allocate(*device_, 1u, /*alignment=*/256u, capPlusOne));
    EXPECT_EQ(budget->residentBytes(), kInitialBytes)
        << "A rejected grow must not charge or allocate its next chunk.";
  }

  EXPECT_EQ(budget->residentBytes(), 0u)
      << "Destroying the grow-only slab must release its persistent reservation.";
}

TEST_F(GeoEncoderTest, RecordSlabRejectsGrowthPastExactDocumentBudget) {
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();
  constexpr uint64_t kInitialBytes = 262144u;
  constexpr std::size_t kInitialSlots = kInitialBytes / sizeof(InstanceRecord);
  budget->setLimitsForTesting({.cacheBytes = 64u << 20, .residentBytes = kInitialBytes});

  {
    GeodeRecordSlab slab(device_->deviceId(), budget);
    GeodeRecordSlab::Slot slot;
    for (std::size_t i = 0; i < kInitialSlots; ++i) {
      ASSERT_TRUE(slab.allocateSlot(*device_, slot)) << "slot " << i;
    }
    EXPECT_EQ(budget->residentBytes(), kInitialBytes);
    EXPECT_FALSE(slab.allocateSlot(*device_, slot));
    EXPECT_EQ(budget->residentBytes(), kInitialBytes);
  }

  EXPECT_EQ(budget->residentBytes(), 0u);
}

TEST(GeodeResourceBudgetTest, CacheReplacementIsAtomicAndReleasesAtOwnerDestruction) {
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();
  budget->setLimitsForTesting({.cacheBytes = 100u, .residentBytes = 100u});

  {
    GeodeGeometryCacheReservation reservation;
    ASSERT_TRUE(reservation.replace(budget, 60u));
    EXPECT_EQ(budget->cacheBytes(), 60u);
    ASSERT_TRUE(reservation.replace(budget, 100u));
    EXPECT_EQ(budget->cacheBytes(), 100u);
    EXPECT_FALSE(reservation.replace(budget, 101u));
    EXPECT_EQ(reservation.bytes(), 100u);
    EXPECT_EQ(budget->cacheBytes(), 100u)
        << "A cap+1 replacement must preserve the previously admitted entry.";
  }

  EXPECT_EQ(budget->cacheBytes(), 0u);
}

TEST(GeodeResourceBudgetTest, ResidentSlotMirrorReplacementPreservesExactReservation) {
  constexpr uint64_t kUniformBytes = 100u;
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();
  budget->setLimitsForTesting({.cacheBytes = kUniformBytes, .residentBytes = 64u << 20});
  auto slab = std::make_shared<GeodeResidentSlab>(/*deviceId=*/1u, budget);

  {
    GeodeResidentSlot slot;
    slot.slab = slab;
    ASSERT_TRUE(slot.reserveUniformMirror(kUniformBytes));
    EXPECT_FALSE(slot.reservePaintMirror(1u));
    EXPECT_EQ(budget->cacheBytes(), kUniformBytes)
        << "A cap+1 mirror must preserve the previously admitted reservation.";
  }

  EXPECT_EQ(budget->cacheBytes(), 0u);
}

TEST(GeodeResourceBudgetTest, ResidentGradientMirrorReleasesAtOwnerDestruction) {
  constexpr uint64_t kUniformBytes = 672u;
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();
  budget->setLimitsForTesting({.cacheBytes = kUniformBytes, .residentBytes = 64u << 20});
  auto slab = std::make_shared<GeodeResidentSlab>(/*deviceId=*/1u, budget);

  {
    GeodeResidentGradientSlot slot;
    slot.slab = slab;
    ASSERT_TRUE(slot.reserveUniformMirror(kUniformBytes));
    EXPECT_FALSE(slot.reserveUniformMirror(kUniformBytes + 1u));
    EXPECT_EQ(budget->cacheBytes(), kUniformBytes);
  }

  EXPECT_EQ(budget->cacheBytes(), 0u);
}

TEST_F(GeoEncoderTest, BatchUniformCpuMirrorStopsAtCapPlusOneAndReleases) {
  constexpr uint64_t kUniformBytes = 16u;
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();

  {
    GeodeRecordSlab slab(device_->deviceId(), budget);
    const uint32_t first[4] = {1u, 2u, 3u, 4u};
    const uint32_t second[4] = {5u, 6u, 7u, 8u};
    ASSERT_TRUE(slab.acquireBatchUniform(*device_, first, sizeof(first)).buffer);
    const uint64_t entryBytes = budget->cacheBytes();
    const uint64_t payloadBytes = slab.batchUniformPayloadBytesForTesting();
    budget->setLimitsForTesting(
        {.cacheBytes = entryBytes + payloadBytes - 1u, .residentBytes = kUniformBytes * 2u});
    EXPECT_FALSE(slab.acquireBatchUniform(*device_, second, sizeof(second)).buffer);
    EXPECT_EQ(budget->cacheBytes(), entryBytes);
    EXPECT_EQ(budget->residentBytes(), kUniformBytes)
        << "The rejected CPU mirror must roll back its tentative GPU reservation.";
  }

  EXPECT_EQ(budget->cacheBytes(), 0u);
  EXPECT_EQ(budget->residentBytes(), 0u);
}

TEST_F(GeoEncoderTest, BatchUniformCpuReservationIncludesVectorSpareCapacity) {
  constexpr uint64_t kUniformBytes = 16u;
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();

  {
    GeodeRecordSlab slab(device_->deviceId(), budget);
    for (uint32_t index = 0; index < 5u; ++index) {
      const uint32_t value[4] = {index, index + 1u, index + 2u, index + 3u};
      ASSERT_TRUE(slab.acquireBatchUniform(*device_, value, sizeof(value)).buffer);
    }
    EXPECT_GE(slab.batchUniformPayloadBytesForTesting(), 5u * kUniformBytes);
    EXPECT_EQ(budget->cacheBytes(), slab.batchUniformMetadataBytesForTesting() +
                                        slab.batchUniformPayloadBytesForTesting())
        << "The family reservation must include spare vector slots, not only live entries.";
  }

  EXPECT_EQ(budget->cacheBytes(), 0u);
}

TEST(GeodeResourceBudgetTest, ChargedResidentMirrorsMoveAndReleaseExactlyOnce) {
  constexpr uint64_t kSolidBytes = 100u;
  constexpr uint64_t kGradientBytes = 200u;
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();
  auto slab = std::make_shared<GeodeResidentSlab>(/*deviceId=*/1u, budget);

  {
    GeodeResidentSlot solid;
    solid.slab = slab;
    ASSERT_TRUE(solid.reserveUniformMirror(kSolidBytes));
    GeodeResidentSlot movedSolid(std::move(solid));
    GeodeResidentSlot assignedSolid;
    assignedSolid = std::move(movedSolid);

    GeodeResidentGradientSlot gradient;
    gradient.slab = slab;
    ASSERT_TRUE(gradient.reserveUniformMirror(kGradientBytes));
    GeodeResidentGradientSlot movedGradient(std::move(gradient));
    GeodeResidentGradientSlot assignedGradient;
    assignedGradient = std::move(movedGradient);

    EXPECT_EQ(budget->cacheBytes(), kSolidBytes + kGradientBytes);
  }

  EXPECT_EQ(budget->cacheBytes(), 0u);
}

TEST(GeodeResourceBudgetTest, ResidentRejectionPreservesCpuCacheFallback) {
  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();
  budget->setLimitsForTesting({.cacheBytes = 100u, .residentBytes = 0u});

  EXPECT_FALSE(budget->reserveResidentBytes(1u));
  GeodeGeometryCacheReservation reservation;
  EXPECT_TRUE(reservation.replace(budget, 100u));
  EXPECT_EQ(budget->cacheBytes(), 100u);
  EXPECT_EQ(budget->residentBytes(), 0u);
}

TEST(GeodeResourceBudgetTest, StrokeCacheReplacementIncludesRetainedDashCapacity) {
  GeodePathCacheComponent::StrokeSlot previous;
  previous.strokeKey.dashArray.reserve(1u);
  const std::optional<std::size_t> previousBytes = previous.retainedBytes();
  ASSERT_TRUE(previousBytes.has_value());

  GeodePathCacheComponent::StrokeSlot replacement;
  replacement.strokeKey.dashArray.reserve(previous.strokeKey.dashArray.capacity() + 1u);
  const std::optional<std::size_t> replacementBytes = replacement.retainedBytes();
  ASSERT_TRUE(replacementBytes.has_value());
  ASSERT_GT(*replacementBytes, *previousBytes);

  auto budget = std::make_shared<GeodeDocumentGeometryBudget>();
  budget->setLimitsForTesting({.cacheBytes = *previousBytes, .residentBytes = 64u << 20});
  GeodeGeometryCacheReservation reservation;
  ASSERT_TRUE(reservation.replace(budget, *previousBytes));
  EXPECT_FALSE(reservation.replace(budget, *replacementBytes));
  EXPECT_EQ(reservation.bytes(), *previousBytes);
  EXPECT_EQ(budget->cacheBytes(), *previousBytes);
}

TEST(GeodeResourceBudgetTest, FrameGeometryStopsAtExactAggregateBoundary) {
  GeodeFrameGeometryBudget budget;
  budget.setLimitsForTesting({.draws = 2u, .items = 5u, .retainedBytes = 7u});

  ASSERT_TRUE(budget.reserve(/*draws=*/1u, /*items=*/2u, /*retainedBytes=*/3u));
  ASSERT_TRUE(budget.reserve(/*draws=*/1u, /*items=*/3u, /*retainedBytes=*/4u));
  EXPECT_FALSE(budget.reserve(/*draws=*/1u, /*items=*/1u, /*retainedBytes=*/1u));
  EXPECT_EQ(budget.draws(), 2u);
  EXPECT_EQ(budget.items(), 5u);
  EXPECT_EQ(budget.retainedBytes(), 7u);
}

/// The scene-batch bind-group cache lives on the device, which outlives the
/// documents drawn through it (renderers lease devices from an idle pool).
/// A destroyed document's slabs release their buffer handles, and those
/// addresses go straight back to the allocator, so a later document's
/// buffers can land on them. Small documents make that collision easy -
/// same chunk size, record offset 0, same record-span byte count - so a key
/// built from handle addresses could compare EQUAL across two unrelated
/// documents and the lookup would return the dead document's bind group,
/// drawing its records and geometry instead. Buffer ids must therefore never
/// repeat, no matter what the allocator does with the handles.
TEST_F(GeoEncoderTest, BufferIdsAreNeverReusedAcrossSlabGenerations) {
  struct SlabGeneration {
    WGPUBuffer chunkHandle = nullptr;
    WGPUBuffer recordHandle = nullptr;
    WGPUBuffer uniformHandle = nullptr;
    uint64_t chunkId = 0;
    uint64_t recordId = 0;
    uint64_t uniformId = 0;
  };

  // One document's worth of slabs, destroyed before returning: exactly the
  // lifetime a rendered-and-closed document has.
  const auto renderOneDocument = [this]() {
    geode::GeodeResidentSlab geometry(device_->deviceId());
    geode::GeodeRecordSlab records(device_->deviceId());

    geode::GeodeResidentSlab::Allocation geometryAlloc;
    EXPECT_TRUE(geometry.allocate(*device_, 4096, 256, geometryAlloc));
    geode::GeodeRecordSlab::Slot recordSlot;
    EXPECT_TRUE(records.allocateSlot(*device_, recordSlot));
    const uint32_t uniformBytes[4] = {1u, 2u, 3u, 4u};
    const geode::GeodeRecordSlab::BatchUniformHandle uniform =
        records.acquireBatchUniform(*device_, uniformBytes, sizeof(uniformBytes));

    SlabGeneration out;
    out.chunkHandle = geometryAlloc.buffer;
    out.recordHandle = recordSlot.buffer;
    out.uniformHandle = uniform.buffer;
    out.chunkId = geometryAlloc.bufferId;
    out.recordId = recordSlot.bufferId;
    out.uniformId = uniform.bufferId;
    return out;
  };

  const SlabGeneration first = renderOneDocument();
  const SlabGeneration second = renderOneDocument();

  // 0 is the "no buffer" sentinel and must never be handed out.
  EXPECT_NE(first.chunkId, 0u);
  EXPECT_NE(first.recordId, 0u);
  EXPECT_NE(first.uniformId, 0u);

  EXPECT_NE(first.chunkId, second.chunkId);
  EXPECT_NE(first.recordId, second.recordId);
  EXPECT_NE(first.uniformId, second.uniformId);

  // Two keys that agree on every size and offset - the shape two small
  // documents rendered back to back produce - must stay distinct. Under the
  // map's ordering, "distinct" means one sorts before the other.
  const auto makeKey = [](const SlabGeneration& generation) {
    geode::GeodeDevice::SceneBatchBindGroupKey key;
    key.uniformBufferId = generation.uniformId;
    key.uniformOffset = 0;
    key.uniformSize = sizeof(geode::InstanceRecord);
    key.chunkBufferId = generation.chunkId;
    key.chunkBytes = 1u << 20;
    key.recordBufferId = generation.recordId;
    key.recordOffset = 0;
    key.recordBytes = 2 * sizeof(geode::InstanceRecord);
    return key;
  };
  const geode::GeodeDevice::SceneBatchBindGroupKey firstKey = makeKey(first);
  const geode::GeodeDevice::SceneBatchBindGroupKey secondKey = makeKey(second);
  EXPECT_TRUE(firstKey < secondKey || secondKey < firstKey)
      << "Two documents' scene-batch keys collided; the second document would "
         "draw the first's records and geometry";

  // Within one slab generation the ids DO have to be stable, or the cache
  // would miss every frame and the batched draw would rebuild its bind group
  // each time.
  geode::GeodeRecordSlab records(device_->deviceId());
  geode::GeodeRecordSlab::Slot slotA;
  geode::GeodeRecordSlab::Slot slotB;
  ASSERT_TRUE(records.allocateSlot(*device_, slotA));
  ASSERT_TRUE(records.allocateSlot(*device_, slotB));
  ASSERT_EQ(slotA.buffer, slotB.buffer) << "Fixture must keep both slots in one chunk";
  EXPECT_EQ(slotA.bufferId, slotB.bufferId);
}

/// End-to-end guard on the scene-batch bind-group cache lookup itself.
///
/// `BufferIdsAreNeverReusedAcrossSlabGenerations` pins the ids, but nothing
/// forces `fillPathSceneBatch` to actually build its key out of them: keying
/// on the `WGPUBuffer` handle addresses instead still passes every other test
/// in this suite, every golden, and the whole resvg suite on any machine
/// whose allocator happens not to recycle the handles. That is precisely the
/// hole the original defect lived in, so the cache lookup gets its own
/// assertion.
///
/// Two slab generations model two documents rendered through one pooled
/// device, shaped so their bindings agree on everything the key can see -
/// same chunk size, record offset 0, same record-span byte count, same
/// uniform bytes. The first generation is fully destroyed before the second
/// allocates, which is what frees the handle addresses for reuse. Every
/// generation's first batch must MISS (one `createBindGroup`), because it
/// binds different buffers; an immediate repeat within a generation must HIT
/// (zero), because it binds the same ones.
TEST_F(GeoEncoderTest, SceneBatchBindGroupCacheDistinguishesSlabGenerations) {
  // One document's slabs, plus the two record slots its batch draws.
  struct Generation {
    std::shared_ptr<geode::GeodeResidentSlab> geometry;
    std::shared_ptr<geode::GeodeRecordSlab> records;
    geode::GeodeResidentSlab::Allocation chunk;
    geode::GeodeRecordSlab::Slot recordSlots[2];
  };

  constexpr uint32_t kInstanceCount = 2;

  const auto makeGeneration = [this]() {
    Generation generation;
    generation.geometry = std::make_shared<geode::GeodeResidentSlab>(device_->deviceId());
    generation.records = std::make_shared<geode::GeodeRecordSlab>(device_->deviceId());
    // 4 KiB of geometry is enough to own a chunk; the chunk's SIZE is what
    // the key sees, and the slab's initial chunk is the same for every
    // generation.
    EXPECT_TRUE(generation.geometry->allocate(*device_, 4096, 256, generation.chunk));
    for (uint32_t i = 0; i < kInstanceCount; ++i) {
      EXPECT_TRUE(generation.records->allocateSlot(*device_, generation.recordSlots[i]));
    }

    // Populate the records so the batch is a real draw rather than a draw
    // over uninitialized storage. Identity transform, a small quad bounding
    // polygon, and zero band counts, so the shader's band-count gates stop
    // before dereferencing the (empty) geometry chunk.
    for (uint32_t i = 0; i < kInstanceCount; ++i) {
      geode::InstanceRecord record = {};
      record.transformRow0[0] = 1.0f;
      record.transformRow1[1] = 1.0f;
      record.color[1] = 0.5f;
      record.color[3] = 1.0f;
      record.boundingVertexCount = 4;
      const float quad[8] = {8.0f, 8.0f, 24.0f, 8.0f, 24.0f, 24.0f, 8.0f, 24.0f};
      std::memcpy(record.boundingVertices, quad, sizeof(quad));
      device_->queue().writeBuffer(generation.recordSlots[i].buffer,
                                   generation.recordSlots[i].offset, &record, sizeof(record));
    }
    return generation;
  };

  const auto bindingFor = [](const Generation& generation) {
    GeoEncoder::SceneBatchBinding binding = {};
    binding.chunkBuffer = generation.chunk.buffer;
    binding.recordBuffer = generation.recordSlots[0].buffer;
    binding.chunkBufferId = generation.chunk.bufferId;
    binding.recordBufferId = generation.recordSlots[0].bufferId;
    binding.firstRecordOffset = generation.recordSlots[0].offset;
    binding.instanceCount = kInstanceCount;
    binding.vertexCount = 6;
    binding.recordSlab = generation.records.get();
    return binding;
  };

  geode::GeodeCounters counters;
  device_->setCounters(&counters);
  // The device is shared process-wide by this fixture, so the counters must
  // come back off however this test exits.
  struct CounterScope {
    geode::GeodeDevice* device;
    ~CounterScope() { device->setCounters(nullptr); }
  } counterScope{device_.get()};

  // Draw one generation's batch twice through a fresh encoder, and report the
  // `createBindGroup` calls each of the two batches caused.
  const auto drawGeneration = [&](const Generation& generation, uint64_t& firstBatchCreates,
                                  uint64_t& repeatBatchCreates) {
    GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
    encoder.clear(css::RGBA(0, 0, 0, 0));

    const GeoEncoder::SceneBatchBinding binding = bindingFor(generation);
    const uint64_t beforeFirst = counters.bindgroupCreates;
    encoder.fillPathSceneBatch(css::RGBA(0, 128, 0, 255), FillRule::NonZero, binding);
    firstBatchCreates = counters.bindgroupCreates - beforeFirst;

    const uint64_t beforeRepeat = counters.bindgroupCreates;
    encoder.fillPathSceneBatch(css::RGBA(0, 128, 0, 255), FillRule::NonZero, binding);
    repeatBatchCreates = counters.bindgroupCreates - beforeRepeat;

    encoder.finish();
  };

  // Several generations, because a handle address only becomes reusable once
  // its owner is gone: one round is a single roll of the allocator's dice,
  // and a key built from addresses has to survive every one of them.
  constexpr int kGenerations = 8;
  for (int round = 0; round < kGenerations; ++round) {
    uint64_t firstBatchCreates = 0;
    uint64_t repeatBatchCreates = 0;
    {
      const Generation generation = makeGeneration();
      drawGeneration(generation, firstBatchCreates, repeatBatchCreates);
    }
    // The generation (and its buffers) are released here, before the next
    // round allocates.

    EXPECT_EQ(firstBatchCreates, 1u)
        << "Round " << round
        << ": this generation's batch binds buffers no earlier generation owned, so its "
           "bind-group lookup must miss. A hit means the cache matched a dead generation's "
           "entry and this batch drew the previous generation's records and geometry.";
    EXPECT_EQ(repeatBatchCreates, 0u)
        << "Round " << round << ": repeating the identical batch must reuse the cached bind group.";
  }
}

/// Companion to the slab-generation test above, and the half of the guard
/// that does not depend on the allocator at all.
///
/// The encoder's bump arenas hand their buffers back to a `GeodeBufferPool`
/// when they are destroyed, and the next encoder reacquires the very same
/// buffer. So two batches drawn through two encoders over ONE generation of
/// slabs agree on every byte a bind-group key can see - same chunk, same
/// record span, same arena buffer at the same offset and size - and differ
/// only in the arena's identity, which is re-stamped on every growth because
/// the recycled buffer's contents start over. The second batch must still
/// miss. `bufferCreates == 0` on the second batch is what proves the pool
/// really did hand back the same buffer, so this test cannot quietly stop
/// testing anything.
TEST_F(GeoEncoderTest, SceneBatchBindGroupCacheDistinguishesRecycledArenaUniforms) {
  auto geometry = std::make_shared<geode::GeodeResidentSlab>(device_->deviceId());
  auto records = std::make_shared<geode::GeodeRecordSlab>(device_->deviceId());

  geode::GeodeResidentSlab::Allocation chunk;
  ASSERT_TRUE(geometry->allocate(*device_, 4096, 256, chunk));
  geode::GeodeRecordSlab::Slot recordSlot;
  ASSERT_TRUE(records->allocateSlot(*device_, recordSlot));

  geode::InstanceRecord record = {};
  record.transformRow0[0] = 1.0f;
  record.transformRow1[1] = 1.0f;
  record.color[1] = 0.5f;
  record.color[3] = 1.0f;
  record.boundingVertexCount = 4;
  const float quad[8] = {8.0f, 8.0f, 24.0f, 8.0f, 24.0f, 24.0f, 8.0f, 24.0f};
  std::memcpy(record.boundingVertices, quad, sizeof(quad));
  device_->queue().writeBuffer(recordSlot.buffer, recordSlot.offset, &record, sizeof(record));

  // A null `recordSlab` routes the batch uniform through the encoder's
  // per-frame arena instead of the slab's persistent table, which is the
  // allocation the buffer pool recycles.
  GeoEncoder::SceneBatchBinding binding = {};
  binding.chunkBuffer = chunk.buffer;
  binding.recordBuffer = recordSlot.buffer;
  binding.chunkBufferId = chunk.bufferId;
  binding.recordBufferId = recordSlot.bufferId;
  binding.firstRecordOffset = recordSlot.offset;
  binding.instanceCount = 1;
  binding.vertexCount = 6;
  binding.recordSlab = nullptr;

  geode::GeodeCounters counters;
  device_->setCounters(&counters);
  struct CounterScope {
    geode::GeodeDevice* device;
    ~CounterScope() { device->setCounters(nullptr); }
  } counterScope{device_.get()};

  GeodeBufferPool pool;
  const auto drawOneEncoder = [&](uint64_t& bindGroupCreates, uint64_t& bufferCreates) {
    GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
    encoder.setBufferPool(&pool);
    encoder.clear(css::RGBA(0, 0, 0, 0));

    const uint64_t bindGroupsBefore = counters.bindgroupCreates;
    const uint64_t buffersBefore = counters.bufferCreates;
    encoder.fillPathSceneBatch(css::RGBA(0, 128, 0, 255), FillRule::NonZero, binding);
    bindGroupCreates = counters.bindgroupCreates - bindGroupsBefore;
    bufferCreates = counters.bufferCreates - buffersBefore;

    encoder.finish();
    // The encoder is destroyed here, releasing its arena buffer into `pool`.
  };

  uint64_t firstBindGroups = 0;
  uint64_t firstBuffers = 0;
  drawOneEncoder(firstBindGroups, firstBuffers);
  EXPECT_EQ(firstBindGroups, 1u);
  ASSERT_GE(firstBuffers, 1u) << "The first batch must grow the uniform arena";

  uint64_t secondBindGroups = 0;
  uint64_t secondBuffers = 0;
  drawOneEncoder(secondBindGroups, secondBuffers);
  ASSERT_EQ(secondBuffers, 0u)
      << "Fixture precondition: the second encoder must reacquire the first encoder's arena "
         "buffer from the pool, otherwise this test is not exercising a recycled buffer";
  EXPECT_EQ(secondBindGroups, 1u)
      << "The recycled arena buffer holds a different uniform allocation than the one the "
         "cached bind group was built over, so the lookup must miss. A hit means the key is "
         "built from the buffer's handle rather than its identity.";
}

TEST_F(GeoEncoderTest, ArenaGrowthKeepsEarlierGridBinding) {
  PathBuilder builder;
  for (int i = 0; i < 8500; ++i) {
    builder.addRect(Box2d({31, 31}, {33, 33}));
  }
  const Path path = builder.build();

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPath(path, css::RGBA(255, 0, 0, 255), FillRule::NonZero);
  encoder.finish();

  const auto pixels = readback();
  EXPECT_THAT(pixelAt(pixels, 32, 32), RgbaEq(255, 0, 0, 255));
}

/// Fill a triangle. Verify center inside, far corners outside.
TEST_F(GeoEncoderTest, FillTriangle) {
  Path path = PathBuilder()
                  .moveTo(Vector2d(32, 8))
                  .lineTo(Vector2d(56, 56))
                  .lineTo(Vector2d(8, 56))
                  .closePath()
                  .build();

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPath(path, css::RGBA(0, 255, 0, 255), FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Center of triangle (32, 40) should be green.
  auto inside = pixelAt(pixels, 32, 40);
  EXPECT_THAT(inside, Rgba(testing::_, testing::Eq(255), testing::_, testing::_))
      << "Triangle center should be green";

  // Top-left and top-right corners are outside the triangle.
  auto topLeft = pixelAt(pixels, 4, 4);
  EXPECT_THAT(topLeft, Rgba(testing::_, testing::Eq(0), testing::_, testing::_))
      << "Top-left corner should stay outside the green triangle";
  auto topRight = pixelAt(pixels, 60, 4);
  EXPECT_THAT(topRight, Rgba(testing::_, testing::Eq(0), testing::_, testing::_))
      << "Top-right corner should stay outside the green triangle";
}

/// Fill a rectangle with a horizontal red→blue linear gradient in user space.
/// Left edge should be red, right edge should be blue, midpoint between.
/// Regression guard for the linear-gradient pipeline plumbing.
TEST_F(GeoEncoderTest, FillLinearGradientUserSpace) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {64, 64})).build();

  LinearGradientParams::Stop stops[2] = {};
  // Red at t=0.
  stops[0].offset = 0.0f;
  stops[0].rgba[0] = 1.0f;
  stops[0].rgba[1] = 0.0f;
  stops[0].rgba[2] = 0.0f;
  stops[0].rgba[3] = 1.0f;
  // Blue at t=1.
  stops[1].offset = 1.0f;
  stops[1].rgba[0] = 0.0f;
  stops[1].rgba[1] = 0.0f;
  stops[1].rgba[2] = 1.0f;
  stops[1].rgba[3] = 1.0f;

  LinearGradientParams params;
  params.startGrad = Vector2d(0.0, 0.0);
  params.endGrad = Vector2d(64.0, 0.0);
  params.gradientFromPath = Transform2d();  // Identity: path space == gradient space.
  params.spreadMode = 0;                    // pad
  params.stops = std::span<const LinearGradientParams::Stop>(stops, 2);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPathLinearGradient(path, params, FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Left edge: almost all red.
  auto left = pixelAt(pixels, 1, 32);
  EXPECT_THAT(left, Rgba(testing::Gt(240), testing::_, testing::Lt(16), testing::_))
      << "Left edge should be mostly red";

  // Right edge: almost all blue.
  auto right = pixelAt(pixels, 62, 32);
  EXPECT_THAT(right, Rgba(testing::Lt(16), testing::_, testing::Gt(240), testing::_))
      << "Right edge should be mostly blue";

  // Midpoint: ~50/50 red/blue.
  auto mid = pixelAt(pixels, 32, 32);
  EXPECT_THAT(mid, Rgba(Near(128, 20), testing::_, Near(128, 20), testing::_))
      << "Midpoint should mix red and blue";
}

/// Gradient `spreadMode=repeat`: start=(16, 0) end=(32, 0) sampled across a
/// 64-wide rect. Outside [16, 32] the gradient should repeat, so pixel at
/// x=16 and x=48 should be (approximately) the same color.
TEST_F(GeoEncoderTest, FillLinearGradientRepeat) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {64, 64})).build();

  LinearGradientParams::Stop stops[2] = {};
  stops[0].offset = 0.0f;
  stops[0].rgba[0] = 1.0f;
  stops[0].rgba[3] = 1.0f;
  stops[1].offset = 1.0f;
  stops[1].rgba[2] = 1.0f;
  stops[1].rgba[3] = 1.0f;

  LinearGradientParams params;
  params.startGrad = Vector2d(16.0, 0.0);
  params.endGrad = Vector2d(32.0, 0.0);
  params.gradientFromPath = Transform2d();
  params.spreadMode = 2;  // repeat
  params.stops = std::span<const LinearGradientParams::Stop>(stops, 2);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPathLinearGradient(path, params, FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // x=17 and x=33 should both sit at t ≈ (1/16) in their respective periods.
  auto a = pixelAt(pixels, 17, 32);
  auto b = pixelAt(pixels, 33, 32);
  EXPECT_THAT(a, Rgba(Near(b[0], 8), testing::_, Near(b[2], 8), testing::_))
      << "Repeat period mismatch; comparison sample b=" << FormatRgba(b);
}

/// Concentric radial gradient (white center → black rim) over a 64x64 rect.
/// Center pixel must be (nearly) white, rim pixels must be (nearly) black.
TEST_F(GeoEncoderTest, FillRadialGradientConcentric) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {64, 64})).build();

  RadialGradientParams::Stop stops[2] = {};
  // White at t=0.
  stops[0].offset = 0.0f;
  stops[0].rgba[0] = 1.0f;
  stops[0].rgba[1] = 1.0f;
  stops[0].rgba[2] = 1.0f;
  stops[0].rgba[3] = 1.0f;
  // Black at t=1.
  stops[1].offset = 1.0f;
  stops[1].rgba[0] = 0.0f;
  stops[1].rgba[1] = 0.0f;
  stops[1].rgba[2] = 0.0f;
  stops[1].rgba[3] = 1.0f;

  RadialGradientParams params;
  params.center = Vector2d(32.0, 32.0);
  params.focalCenter = Vector2d(32.0, 32.0);
  params.radius = 32.0;
  params.focalRadius = 0.0;
  params.gradientFromPath = Transform2d();
  params.spreadMode = 0;  // pad
  params.stops = std::span<const RadialGradientParams::Stop>(stops, 2);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPathRadialGradient(path, params, FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Center pixel (32, 32) sits exactly at the focal point → t == 0 → white.
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_THAT(center, Rgba(testing::Gt(240), testing::Gt(240), testing::Gt(240), testing::_))
      << "Center should be nearly white";

  // A point one pixel inside the rim (left edge) sits very close to t == 1
  // along the +x ray from the center → essentially black.
  auto leftRim = pixelAt(pixels, 1, 32);
  EXPECT_THAT(leftRim, Rgba(testing::Lt(32), testing::Lt(32), testing::Lt(32), testing::_))
      << "Left rim should be nearly black";
}

/// Off-center focal point: brightest pixel should be at the focal point,
/// not at the geometric center of the outer circle.
TEST_F(GeoEncoderTest, FillRadialGradientFocal) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {64, 64})).build();

  RadialGradientParams::Stop stops[2] = {};
  stops[0].offset = 0.0f;
  stops[0].rgba[0] = 1.0f;
  stops[0].rgba[1] = 1.0f;
  stops[0].rgba[2] = 1.0f;
  stops[0].rgba[3] = 1.0f;
  stops[1].offset = 1.0f;
  stops[1].rgba[0] = 0.0f;
  stops[1].rgba[1] = 0.0f;
  stops[1].rgba[2] = 0.0f;
  stops[1].rgba[3] = 1.0f;

  RadialGradientParams params;
  params.center = Vector2d(32.0, 32.0);
  params.focalCenter = Vector2d(48.0, 32.0);  // Focal shifted toward +x.
  params.radius = 30.0;
  params.focalRadius = 0.0;
  params.gradientFromPath = Transform2d();
  params.spreadMode = 0;
  params.stops = std::span<const RadialGradientParams::Stop>(stops, 2);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPathRadialGradient(path, params, FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // The pixel right at the focal point should be the lightest. The
  // geometric center of the outer circle should be noticeably darker.
  auto focal = pixelAt(pixels, 48, 32);
  auto geomCenter = pixelAt(pixels, 32, 32);
  EXPECT_THAT(focal, Rgba(testing::Gt(geomCenter[0] + 32), testing::Gt(geomCenter[1] + 32),
                          testing::Gt(geomCenter[2] + 32), testing::_))
      << "Focal point should be lighter than geometric center " << FormatRgba(geomCenter);
}

/// Fill a circle. Verify center inside, far corners outside.
TEST_F(GeoEncoderTest, FillCircle) {
  Path path = PathBuilder().addCircle(Vector2d(32, 32), 20).build();

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.fillPath(path, css::RGBA(0, 0, 255, 255), FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Center should be blue.
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_THAT(center, Rgba(testing::_, testing::_, testing::Eq(255), testing::_))
      << "Center should be blue";

  // Far corner should be black (outside the circle).
  auto corner = pixelAt(pixels, 2, 2);
  EXPECT_THAT(corner, Rgba(testing::_, testing::_, testing::Eq(0), testing::_))
      << "Corner should stay outside the blue circle";
}

/// Build a 2x2 solid-magenta straight-alpha RGBA8 image resource.
svg::ImageResource makeMagentaImage2x2() {
  svg::ImageResource image;
  image.width = 2;
  image.height = 2;
  image.data = {
      255, 0, 255, 255, 255, 0, 255, 255,  //
      255, 0, 255, 255, 255, 0, 255, 255,
  };
  return image;
}

/// Draw a 2x2 magenta image stretched to fill a large destination rectangle.
/// Verify that the destination pixels are magenta and the outside is
/// untouched.
TEST_F(GeoEncoderTest, DrawImageFillsDestRect) {
  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));

  svg::ImageResource image = makeMagentaImage2x2();
  encoder.drawImage(image, Box2d({16.0, 16.0}, {48.0, 48.0}),
                    /*opacity=*/1.0, /*pixelated=*/true);
  encoder.finish();

  auto pixels = readback();
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_THAT(center, RgbaEq(255, 0, 255, 255)) << "Center should be magenta";

  // Outside the destRect is still the clear color (black).
  auto outside = pixelAt(pixels, 4, 4);
  EXPECT_THAT(outside, Rgba(testing::Eq(0), testing::Eq(0), testing::Eq(0), testing::_))
      << "Outside destination should stay clear black";
}

/// `opacity` should blend the image with whatever the pass already
/// contains. Start with a black background, draw a red image at 50%
/// opacity - the result should read back as ~(128, 0, 0, 255) over black
/// with premultiplied source-over.
TEST_F(GeoEncoderTest, DrawImageHonorsOpacity) {
  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));

  svg::ImageResource image;
  image.width = 1;
  image.height = 1;
  image.data = {255, 0, 0, 255};  // Red, full alpha.

  encoder.drawImage(image, Box2d({16.0, 16.0}, {48.0, 48.0}),
                    /*opacity=*/0.5, /*pixelated=*/false);
  encoder.finish();

  auto pixels = readback();
  auto center = pixelAt(pixels, 32, 32);
  // 0.5 alpha source over opaque black → R ≈ 128, A = 255.
  EXPECT_THAT(center, Rgba(Near(128, 2), testing::Eq(0), testing::Eq(0), testing::Eq(255)))
      << "Image opacity should blend red over opaque black";
}

TEST_F(GeoEncoderTest, DrawImageOverDeviceTextureLimitIsNoOp) {
  wgpu::Limits limits;
  ASSERT_EQ(device_->device().getLimits(&limits), wgpu::Status::Success);
  ASSERT_LT(limits.maxTextureDimension2D, static_cast<uint32_t>(std::numeric_limits<int>::max()));
  const int overLimitWidth = static_cast<int>(limits.maxTextureDimension2D) + 1;

  svg::ImageResource image;
  image.width = overLimitWidth;
  image.height = 1;
  image.data.resize(static_cast<std::size_t>(overLimitWidth) * 4u, 255);

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  encoder.drawImage(image, Box2d({16.0, 16.0}, {48.0, 48.0}), /*opacity=*/1.0,
                    /*pixelated=*/true);
  encoder.finish();

  EXPECT_THAT(pixelAt(readback(), 32, 32), RgbaEq(0, 0, 0, 255));
}

/// Mixing a fillPath and a drawImage in the same pass must work - after
/// this test shipped, future refactors that forget to re-bind the Slug
/// fill pipeline between pipeline switches will regress here.
TEST_F(GeoEncoderTest, FillThenImageThenFill) {
  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));

  // 1) Fill a red rect covering the top half.
  Path topRect = PathBuilder().addRect(Box2d({0, 0}, {64, 32})).build();
  encoder.fillPath(topRect, css::RGBA(255, 0, 0, 255), FillRule::NonZero);

  // 2) Draw a magenta image in the center.
  svg::ImageResource image = makeMagentaImage2x2();
  encoder.drawImage(image, Box2d({24.0, 24.0}, {40.0, 40.0}),
                    /*opacity=*/1.0, /*pixelated=*/true);

  // 3) Fill a green rect covering the bottom strip (below the image).
  Path bottomRect = PathBuilder().addRect(Box2d({0, 48}, {64, 64})).build();
  encoder.fillPath(bottomRect, css::RGBA(0, 255, 0, 255), FillRule::NonZero);
  encoder.finish();

  auto pixels = readback();

  // Top half: red.
  auto top = pixelAt(pixels, 8, 8);
  EXPECT_THAT(top, Rgba(testing::Eq(255), testing::Eq(0), testing::_, testing::_))
      << "Top half should be red";

  // Center (image): magenta.
  auto mid = pixelAt(pixels, 32, 32);
  EXPECT_THAT(mid, Rgba(testing::Eq(255), testing::_, testing::Eq(255), testing::_))
      << "Center image should be magenta";

  // Bottom strip: green.
  auto bottom = pixelAt(pixels, 8, 56);
  EXPECT_THAT(bottom, Rgba(testing::Eq(0), testing::Eq(255), testing::_, testing::_))
      << "Bottom strip should be green";
}

/// Fill a rectangle sampling a 4x4 solid-red pattern tile. Verifies the
/// pattern-sampling fragment path end-to-end: tile texture upload, Slug
/// winding coverage, and `fract()`-based wrap.
TEST_F(GeoEncoderTest, FillPathPatternSolidTile) {
  // 1. Build a 4x4 solid red RGBA8 pattern tile (premultiplied).
  constexpr uint32_t kTileDim = 4;
  std::array<uint8_t, kTileDim * kTileDim * 4> tilePixels;
  for (size_t i = 0; i < tilePixels.size(); i += 4) {
    tilePixels[i + 0] = 255;  // R
    tilePixels[i + 1] = 0;
    tilePixels[i + 2] = 0;
    tilePixels[i + 3] = 255;
  }
  wgpu::TextureDescriptor td = {};
  td.label = wgpuLabel("PatternTile");
  td.size = {kTileDim, kTileDim, 1};
  td.format = kFormat;
  td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  wgpu::Texture tile = device_->device().createTexture(td);
  ASSERT_TRUE(static_cast<bool>(tile));

  wgpu::TexelCopyTextureInfo dst = {};
  dst.texture = tile;
  wgpu::TexelCopyBufferLayout layout = {};
  layout.bytesPerRow = kTileDim * 4;
  layout.rowsPerImage = kTileDim;
  wgpu::Extent3D extent = {kTileDim, kTileDim, 1};
  device_->queue().writeTexture(dst, tilePixels.data(), tilePixels.size(), layout, extent);

  // 2. Fill a path with the pattern. The tile size in pattern-space is
  // 4x4 so the shader wraps every 4 pixels; since the path spans more than
  // one tile, the wrap logic is exercised.
  Path path = PathBuilder().addRect(Box2d({16, 16}, {48, 48})).build();

  GeoEncoder encoder(*device_, *pipeline_, *gradientPipeline_, *imagePipeline_, target_);
  encoder.clear(css::RGBA(0, 0, 0, 255));
  GeoEncoder::PatternPaint paint;
  paint.tile = tile;
  paint.tileSize = Vector2d(4.0, 4.0);
  paint.patternFromPath = Transform2d();  // Identity: target space == pattern space.
  paint.opacity = 1.0;
  encoder.fillPathPattern(path, FillRule::NonZero, paint);
  encoder.finish();

  auto pixels = readback();
  auto center = pixelAt(pixels, 32, 32);
  EXPECT_THAT(center, RgbaEq(255, 0, 0, 255)) << "Center should sample red pattern";

  // Outside the rect should stay at the clear color (black).
  auto corner = pixelAt(pixels, 4, 4);
  EXPECT_THAT(corner, Rgba(testing::Eq(0), testing::_, testing::_, testing::Eq(255)))
      << "Corner should stay at the opaque clear color";
}

}  // namespace

}  // namespace donner::geode
