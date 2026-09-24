#include "donner/svg/renderer/HeadlessDevicePool.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <thread>

namespace donner::svg::details {
namespace {

/// A device the cache can hand out, which a test can lose.
struct FakeDevice {
  /// Order in which the factory opened it, from 1.
  int serial = 0;
  /// Whether the device has been lost.
  bool lost = false;
  /// Whether the device may only be used on the thread that opened it, as a browser device may.
  bool bound = false;

  [[nodiscard]] bool isDeviceLost() const { return lost; }
  [[nodiscard]] bool isBoundToCreatingThread() const { return bound; }
};

/// A cache over \ref FakeDevice that counts the devices it opened.
struct FakePool {
  /// @param maxIdleDevices Most idle devices the cache keeps.
  explicit FakePool(std::size_t maxIdleDevices)
      : pool(
            [this] {
              auto device = std::make_shared<FakeDevice>();
              device->serial = ++opened;
              return device;
            },
            maxIdleDevices) {}

  int opened = 0;  //!< Devices the factory opened.
  HeadlessDevicePool<FakeDevice> pool;
};

TEST(HeadlessDevicePool, ReusesADeviceItsLastLeaseReleased) {
  FakePool cache(4);
  int firstSerial = 0;
  {
    std::shared_ptr<FakeDevice> first = cache.pool.acquire();
    ASSERT_THAT(first, testing::NotNull());
    firstSerial = first->serial;
  }
  EXPECT_THAT(cache.pool.idleCount(), 1u);

  std::shared_ptr<FakeDevice> second = cache.pool.acquire();
  ASSERT_THAT(second, testing::NotNull());
  EXPECT_THAT(second->serial, firstSerial);
  EXPECT_THAT(cache.opened, 1);
}

TEST(HeadlessDevicePool, NeverHandsOutALostDevice) {
  FakePool cache(4);
  {
    std::shared_ptr<FakeDevice> device = cache.pool.acquire();
    ASSERT_THAT(device, testing::NotNull());
    device->lost = true;
  }
  EXPECT_THAT(cache.pool.idleCount(), 0u) << "a lost device went back into the cache";

  std::shared_ptr<FakeDevice> replacement = cache.pool.acquire();
  ASSERT_THAT(replacement, testing::NotNull());
  EXPECT_THAT(replacement->lost, testing::IsFalse());
  EXPECT_THAT(cache.opened, 2);
}

TEST(HeadlessDevicePool, KeepsNoMoreIdleDevicesThanItsBound) {
  FakePool cache(2);
  {
    std::shared_ptr<FakeDevice> a = cache.pool.acquire();
    std::shared_ptr<FakeDevice> b = cache.pool.acquire();
    std::shared_ptr<FakeDevice> c = cache.pool.acquire();
    ASSERT_THAT(c, testing::NotNull());
  }
  EXPECT_THAT(cache.pool.idleCount(), 2u);
  EXPECT_THAT(cache.opened, 3);
}

TEST(HeadlessDevicePool, NeverHandsADeviceBoundToItsThreadToAnother) {
  FakePool cache(4);
  int boundSerial = 0;
  std::thread elsewhere([&] {
    std::shared_ptr<FakeDevice> device = cache.pool.acquire();
    ASSERT_THAT(device, testing::NotNull());
    device->bound = true;
    boundSerial = device->serial;
  });
  elsewhere.join();
  ASSERT_THAT(cache.pool.idleCount(), 1u);

  // A browser device's objects belong to the worker that made them, so a renderer on this thread
  // handed that device could not draw with it.
  std::shared_ptr<FakeDevice> here = cache.pool.acquire();
  ASSERT_THAT(here, testing::NotNull());
  EXPECT_THAT(here->serial, testing::Ne(boundSerial))
      << "a device bound to the thread that opened it was handed to another thread";
  EXPECT_THAT(cache.opened, 2);
}

TEST(HeadlessDevicePool, HandsABoundDeviceBackToTheThreadThatOpenedIt) {
  FakePool cache(4);
  int boundSerial = 0;
  {
    std::shared_ptr<FakeDevice> device = cache.pool.acquire();
    ASSERT_THAT(device, testing::NotNull());
    device->bound = true;
    boundSerial = device->serial;
  }
  std::shared_ptr<FakeDevice> again = cache.pool.acquire();
  ASSERT_THAT(again, testing::NotNull());
  EXPECT_THAT(again->serial, boundSerial);
  EXPECT_THAT(cache.opened, 1);
}

TEST(HeadlessDevicePool, AnUnboundDeviceMovesBetweenThreads) {
  FakePool cache(4);
  int serial = 0;
  std::thread elsewhere([&] {
    std::shared_ptr<FakeDevice> device = cache.pool.acquire();
    ASSERT_THAT(device, testing::NotNull());
    serial = device->serial;
  });
  elsewhere.join();

  std::shared_ptr<FakeDevice> here = cache.pool.acquire();
  ASSERT_THAT(here, testing::NotNull());
  EXPECT_THAT(here->serial, serial);
  EXPECT_THAT(cache.opened, 1);
}

TEST(HeadlessDevicePool, AFullCacheMakesRoomForTheDeviceReleasedLast) {
  // An idle device bound to a thread that has since exited can never be handed out again, so a
  // full cache gives up its oldest device rather than the one just released.
  FakePool cache(1);
  int newestSerial = 0;
  {
    std::shared_ptr<FakeDevice> older = cache.pool.acquire();
    std::shared_ptr<FakeDevice> newer = cache.pool.acquire();
    ASSERT_THAT(newer, testing::NotNull());
    newestSerial = newer->serial;
    older.reset();
  }
  ASSERT_THAT(cache.pool.idleCount(), 1u);
  std::shared_ptr<FakeDevice> kept = cache.pool.acquire();
  ASSERT_THAT(kept, testing::NotNull());
  EXPECT_THAT(kept->serial, newestSerial);
}

TEST(HeadlessDevicePool, AFactoryThatOpensNothingHandsOutNothing) {
  HeadlessDevicePool<FakeDevice> pool([] { return std::shared_ptr<FakeDevice>(); }, 4);
  EXPECT_THAT(pool.acquire(), testing::IsNull());
  EXPECT_THAT(pool.idleCount(), 0u);
}

}  // namespace
}  // namespace donner::svg::details
