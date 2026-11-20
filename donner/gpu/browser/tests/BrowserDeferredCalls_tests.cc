#include "donner/gpu/browser/BrowserDeferredCalls.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace donner::gpu::browser {
namespace {

using testing::ElementsAre;

/// Runs a round trip's callable at once, as the owner does when it executes the round trip.
const auto kRunNow = [](auto&& callback) { callback(); };

/// A held call that appends \p name to \p ran and answers \p status.
BrowserDeferredCalls::Call Recording(std::vector<std::string>& ran, const char* name,
                                     uint64_t objectId, BridgeStatus status = BridgeStatus::Success,
                                     std::atomic<uint32_t>* sink = nullptr) {
  return BrowserDeferredCalls::Call{name, objectId,
                                    [&ran, name, status] {
                                      ran.emplace_back(name);
                                      return status;
                                    },
                                    sink};
}

TEST(BrowserDeferredCalls, HeldCallsRunBeforeTheRoundTripThatCarriesThemInCallOrder) {
  BrowserDeferredCalls deferred;
  std::vector<std::string> ran;
  EXPECT_FALSE(deferred.add(Recording(ran, "writeBuffer", 1)));
  EXPECT_FALSE(deferred.add(Recording(ran, "createBindGroup", 2)));
  EXPECT_FALSE(deferred.add(Recording(ran, "destroyObject", 3)));

  int roundTrips = 0;
  const auto dispatch = [&](auto&& callback) {
    ++roundTrips;
    callback();
  };
  deferred.runAfter([&] { ran.emplace_back("submitCommandBuffers"); }, dispatch);
  EXPECT_THAT(
      ran, ElementsAre("writeBuffer", "createBindGroup", "destroyObject", "submitCommandBuffers"))
      << "a direct call runs after every call deferred before it";
  EXPECT_EQ(roundTrips, 1) << "the held calls ride in the direct call's own round trip";
  EXPECT_TRUE(deferred.empty());

  deferred.runAfter([&] { ran.emplace_back("createTexture"); }, dispatch);
  EXPECT_THAT(ran.back(), "createTexture");
  EXPECT_EQ(ran.size(), 5u) << "a carried call runs once";
}

TEST(BrowserDeferredCalls, EachKindsRefusalReachesTheDeviceThatMadeIt) {
  for (const char* kind :
       {"writeBuffer", "createBindGroup", "createBindGroupLayout", "destroyObject"}) {
    SCOPED_TRACE(kind);
    BrowserDeferredCalls deferred;
    std::vector<std::string> ran;
    std::atomic<uint32_t> refusingDevice{0};
    std::atomic<uint32_t> otherDevice{0};
    deferred.add(Recording(ran, "writeBuffer", 10, BridgeStatus::Success, &otherDevice));
    deferred.add(Recording(ran, kind, 11, BridgeStatus::UnknownObject, &refusingDevice));
    deferred.add(Recording(ran, "destroyObject", 12, BridgeStatus::Success, &otherDevice));

    const BrowserDeferredCalls::Refusal refusal = deferred.runAfter([] {}, kRunNow);
    EXPECT_EQ(ran.size(), 3u) << "a refusal does not stop the calls after it";
    EXPECT_EQ(refusingDevice.load(), static_cast<uint32_t>(BridgeStatus::UnknownObject));
    EXPECT_EQ(otherDevice.load(), static_cast<uint32_t>(BridgeStatus::Success))
        << "no other device is charged with the refusal";
    EXPECT_STREQ(refusal.operation, kind);
    EXPECT_EQ(refusal.objectId, 11u) << "the diagnostic names the refused call's own object";
    EXPECT_EQ(refusal.status, BridgeStatus::UnknownObject);
    EXPECT_EQ(refusal.count, 1u);
  }
}

TEST(BrowserDeferredCalls, ADevicesFirstRefusalIsKept) {
  BrowserDeferredCalls deferred;
  std::vector<std::string> ran;
  std::atomic<uint32_t> device{0};
  deferred.add(Recording(ran, "createBindGroup", 1, BridgeStatus::WrongObjectKind, &device));
  deferred.add(Recording(ran, "writeBuffer", 2, BridgeStatus::DeviceLost, &device));
  const BrowserDeferredCalls::Refusal refusal = deferred.runAfter([] {}, kRunNow);
  EXPECT_EQ(device.load(), static_cast<uint32_t>(BridgeStatus::WrongObjectKind));
  EXPECT_EQ(refusal.count, 2u);
}

TEST(BrowserDeferredCalls, MustBeSentOnceEitherBoundIsReached) {
  std::vector<std::string> ran;
  BrowserDeferredCalls byCount;
  for (std::size_t held = 1; held < BrowserDeferredCalls::kMaxCalls; ++held) {
    ASSERT_FALSE(byCount.add(Recording(ran, "destroyObject", held))) << held;
  }
  EXPECT_TRUE(byCount.add(Recording(ran, "destroyObject", 0)));
  EXPECT_EQ(byCount.size(), BrowserDeferredCalls::kMaxCalls);

  BrowserDeferredCalls byBytes;
  EXPECT_FALSE(byBytes.add(Recording(ran, "writeBuffer", 1), BrowserDeferredCalls::kMaxBytes - 1));
  EXPECT_TRUE(byBytes.add(Recording(ran, "writeBuffer", 2), 1));
  EXPECT_EQ(byBytes.bytes(), BrowserDeferredCalls::kMaxBytes);
  byBytes.runAfter([] {}, kRunNow);
  EXPECT_EQ(byBytes.bytes(), 0u) << "sending the held calls releases their payload";
  EXPECT_FALSE(byBytes.add(Recording(ran, "writeBuffer", 3), BrowserDeferredCalls::kMaxBytes - 1));
}

TEST(BrowserDeferredCalls, AHeldWriteOwnsACopyOfItsBytes) {
  std::vector<uint8_t> caller = {1, 2, 3, 4};
  std::vector<uint8_t> written;
  std::function<BridgeStatus()> write =
      BrowserDeferredCalls::OwningWrite(caller, [&](std::span<const uint8_t> bytes) {
        written.assign(bytes.begin(), bytes.end());
        return BridgeStatus::Success;
      });
  // The caller's bytes may change or go away as soon as the write is held.
  caller.assign({9, 9, 9, 9});
  caller.clear();
  caller.shrink_to_fit();
  EXPECT_EQ(write(), BridgeStatus::Success);
  EXPECT_THAT(written, ElementsAre(1, 2, 3, 4));
}

}  // namespace
}  // namespace donner::gpu::browser
