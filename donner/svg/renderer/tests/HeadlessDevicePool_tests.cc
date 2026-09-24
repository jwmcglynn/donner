#include "donner/svg/renderer/HeadlessDevicePool.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

namespace donner::svg::details {
namespace {

/// A device the cache can hand out, which a test can lose.
struct FakeDevice {
  /// Order in which the factory opened it, from 1.
  int serial = 0;
  /// Whether the device has been lost.
  bool lost = false;

  [[nodiscard]] bool isDeviceLost() const { return lost; }
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

TEST(HeadlessDevicePool, AFactoryThatOpensNothingHandsOutNothing) {
  HeadlessDevicePool<FakeDevice> pool([] { return std::shared_ptr<FakeDevice>(); }, 4);
  EXPECT_THAT(pool.acquire(), testing::IsNull());
  EXPECT_THAT(pool.idleCount(), 0u);
}

}  // namespace
}  // namespace donner::svg::details
