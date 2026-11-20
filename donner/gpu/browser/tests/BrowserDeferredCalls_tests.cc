#include "donner/gpu/browser/BrowserDeferredCalls.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace donner::gpu::browser {
namespace {

using testing::ElementsAre;

TEST(BrowserDeferredCalls, RunsHeldCallsInCallOrderAndHoldsNothingAfter) {
  BrowserDeferredCalls deferred;
  std::vector<std::string> ran;
  for (const char* name : {"writeBuffer", "createBindGroup", "destroyObject"}) {
    EXPECT_FALSE(deferred.add(name, [&ran, name] {
      ran.emplace_back(name);
      return BridgeStatus::Success;
    }));
  }
  ASSERT_FALSE(deferred.empty());

  BrowserDeferredCalls::Refusal refusal;
  BrowserDeferredCalls::Execute(deferred.take(), refusal);
  EXPECT_THAT(ran, ElementsAre("writeBuffer", "createBindGroup", "destroyObject"));
  EXPECT_EQ(refusal.count, 0u);
  EXPECT_TRUE(deferred.empty()) << "a round trip carries every held call once";
}

TEST(BrowserDeferredCalls, ARefusalIsReportedWithoutStoppingTheCallsAfterIt) {
  BrowserDeferredCalls deferred;
  std::vector<std::string> ran;
  const auto call = [&ran](const char* name, BridgeStatus status) {
    return [&ran, name, status] {
      ran.emplace_back(name);
      return status;
    };
  };
  deferred.add("writeBuffer", call("writeBuffer", BridgeStatus::Success));
  deferred.add("createBindGroup", call("createBindGroup", BridgeStatus::UnknownObject));
  deferred.add("destroyObject", call("destroyObject", BridgeStatus::DeviceLost));
  deferred.add("writeBuffer", call("writeBuffer", BridgeStatus::Success));

  BrowserDeferredCalls::Refusal refusal;
  BrowserDeferredCalls::Execute(deferred.take(), refusal);
  EXPECT_THAT(ran, ElementsAre("writeBuffer", "createBindGroup", "destroyObject", "writeBuffer"))
      << "each held call was an independent call when it was made";
  EXPECT_STREQ(refusal.operation, "createBindGroup");
  EXPECT_EQ(refusal.status, BridgeStatus::UnknownObject);
  EXPECT_EQ(refusal.count, 2u);
}

TEST(BrowserDeferredCalls, MustBeSentOnceEitherBoundIsReached) {
  const auto nothing = [] { return BridgeStatus::Success; };
  BrowserDeferredCalls byCount;
  for (std::size_t held = 1; held < BrowserDeferredCalls::kMaxCalls; ++held) {
    ASSERT_FALSE(byCount.add("destroyObject", nothing)) << held;
  }
  EXPECT_TRUE(byCount.add("destroyObject", nothing));
  EXPECT_EQ(byCount.take().size(), BrowserDeferredCalls::kMaxCalls);

  BrowserDeferredCalls byBytes;
  EXPECT_FALSE(byBytes.add("writeBuffer", nothing, BrowserDeferredCalls::kMaxBytes - 1));
  EXPECT_TRUE(byBytes.add("writeBuffer", nothing, 1));
  EXPECT_EQ(byBytes.take().size(), 2u);
  EXPECT_FALSE(byBytes.add("writeBuffer", nothing, BrowserDeferredCalls::kMaxBytes - 1))
      << "taking the held calls starts the byte count over";
}

}  // namespace
}  // namespace donner::gpu::browser
