/// @file
/// Tests for \ref donner::geode::GeodeHandleRetirement and \ref donner::geode::GeodePerDevice:
/// handles let go of on another thread reach their device only through the owning context, and
/// state kept per device neither mixes devices nor outlives the context it belongs to.

#include "donner/svg/renderer/geode/GeodeHandleRetirement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/geode/GeodePerDevice.h"

using testing::Eq;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsNull;
using testing::IsTrue;
using testing::NotNull;

namespace donner::geode {
namespace {

/// Number of buffers \p device has released.
size_t ReleasedBufferCount(const gpu::RecordingDevice& device) {
  const std::string capture = device.serialize();
  constexpr std::string_view kDestroyBuffer = "destroy buffer#";
  size_t count = 0;
  for (size_t at = capture.find(kDestroyBuffer); at != std::string::npos;
       at = capture.find(kDestroyBuffer, at + kDestroyBuffer.size())) {
    ++count;
  }
  return count;
}

class GeodeHandleRetirementTest : public testing::Test {
protected:
  gpu::Buffer createBuffer() {
    return gpu::GetResultOrFail(device_.createBuffer(gpu::BufferDescriptor{
        "retired", 16, gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst}));
  }

  /// Retires \p buffer alone.
  void retireOne(gpu::Buffer buffer) {
    std::vector<gpu::Buffer> buffers;
    buffers.push_back(std::move(buffer));
    std::vector<gpu::BindGroup> bindGroups;
    retirement_.retire(buffers, bindGroups);
    EXPECT_THAT(buffers, IsEmpty());
  }

  gpu::RecordingDevice device_;
  GeodeHandleRetirement retirement_;
};

TEST_F(GeodeHandleRetirementTest, RetiringHoldsAHandleUntilTheOwnerReleasesIt) {
  retireOne(createBuffer());

  EXPECT_THAT(retirement_.heldCountForTesting(), Eq(1u));
  EXPECT_THAT(ReleasedBufferCount(device_), Eq(0u)) << "retiring must not reach the device";

  retirement_.release();

  EXPECT_THAT(retirement_.heldCountForTesting(), Eq(0u));
  EXPECT_THAT(ReleasedBufferCount(device_), Eq(1u));
}

TEST_F(GeodeHandleRetirementTest, RetiringOnAnotherThreadReachesTheDeviceOnlyOnTheOwner) {
  constexpr size_t kBuffers = 64;
  std::vector<gpu::Buffer> buffers;
  for (size_t i = 0; i < kBuffers; ++i) {
    buffers.push_back(createBuffer());
  }

  // Another thread lets go of the buffers one at a time while the owner keeps releasing, as a
  // thread destroying a document does while the device's own thread renders. Only the owner's
  // thread touches the device.
  std::atomic<bool> retiredAll = false;
  std::thread other([&] {
    for (gpu::Buffer& buffer : buffers) {
      retireOne(std::move(buffer));
    }
    retiredAll.store(true, std::memory_order_release);
  });
  while (!retiredAll.load(std::memory_order_acquire)) {
    retirement_.release();
  }
  other.join();
  retirement_.release();

  EXPECT_THAT(retirement_.heldCountForTesting(), Eq(0u));
  EXPECT_THAT(ReleasedBufferCount(device_), Eq(kBuffers));
}

TEST_F(GeodeHandleRetirementTest, ClosingReleasesWhatIsHeldAndLaterRetirementsWait) {
  retireOne(createBuffer());
  EXPECT_THAT(retirement_.closed(), IsFalse());

  retirement_.close();

  EXPECT_THAT(retirement_.closed(), IsTrue());
  EXPECT_THAT(ReleasedBufferCount(device_), Eq(1u));

  // After the context closed, nothing releases on its behalf: a late retirement is kept until the
  // retirement itself goes, which its owner arranges to be after the device.
  retireOne(createBuffer());
  EXPECT_THAT(retirement_.heldCountForTesting(), Eq(1u));
  EXPECT_THAT(ReleasedBufferCount(device_), Eq(1u));
}

TEST(GeodePerDeviceTest, EachDeviceGetsItsOwnEntry) {
  const auto first = std::make_shared<GeodeHandleRetirement>();
  const auto second = std::make_shared<GeodeHandleRetirement>();
  GeodePerDevice<int> perDevice;

  perDevice.forDevice(GeodeDeviceKey{1, first}) = 10;
  perDevice.forDevice(GeodeDeviceKey{2, second}) = 20;

  EXPECT_THAT(perDevice.size(), Eq(2u));
  EXPECT_THAT(perDevice.forDevice(GeodeDeviceKey{1, first}), Eq(10));
  EXPECT_THAT(perDevice.forDevice(GeodeDeviceKey{2, second}), Eq(20));
  ASSERT_THAT(perDevice.find(2), NotNull());
  EXPECT_THAT(*perDevice.find(2), Eq(20));
  EXPECT_THAT(perDevice.find(3), IsNull());
}

TEST(GeodePerDeviceTest, AnEntryStaysPutWhileOtherDevicesComeAndGo) {
  const auto first = std::make_shared<GeodeHandleRetirement>();
  GeodePerDevice<int> perDevice;
  int& entry = perDevice.forDevice(GeodeDeviceKey{1, first});

  std::vector<std::shared_ptr<GeodeHandleRetirement>> others;
  for (uint64_t deviceId = 2; deviceId < 34; ++deviceId) {
    others.push_back(std::make_shared<GeodeHandleRetirement>());
    (void)perDevice.forDevice(GeodeDeviceKey{deviceId, others.back()});
  }
  others.clear();
  (void)perDevice.forDevice(GeodeDeviceKey{1, first});

  EXPECT_THAT(perDevice.size(), Eq(1u));
  EXPECT_THAT(perDevice.find(1), Eq(&entry));
}

TEST(GeodePerDeviceTest, AGoneContextsEntryIsDroppedAtTheNextLookup) {
  const auto closing = std::make_shared<GeodeHandleRetirement>();
  auto destroyed = std::make_shared<GeodeHandleRetirement>();
  const auto live = std::make_shared<GeodeHandleRetirement>();
  GeodePerDevice<int> perDevice;
  (void)perDevice.forDevice(GeodeDeviceKey{1, closing});
  (void)perDevice.forDevice(GeodeDeviceKey{2, destroyed});
  (void)perDevice.forDevice(GeodeDeviceKey{3, live});

  closing->close();
  destroyed.reset();
  EXPECT_THAT(perDevice.size(), Eq(3u)) << "entries are dropped only when one is looked up";

  (void)perDevice.forDevice(GeodeDeviceKey{3, live});

  EXPECT_THAT(perDevice.find(1), IsNull()) << "a closed context's entry must go";
  EXPECT_THAT(perDevice.find(2), IsNull()) << "a destroyed context's entry must go";
  EXPECT_THAT(perDevice.find(3), NotNull());
}

}  // namespace
}  // namespace donner::geode
