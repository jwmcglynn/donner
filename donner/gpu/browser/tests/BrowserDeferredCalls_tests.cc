#include "donner/gpu/browser/BrowserDeferredCalls.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "donner/gpu/browser/tests/FakeOwnerChannel.h"

namespace donner::gpu::browser {
namespace {

using testing::ElementsAre;

/// A held call that appends \p name to \p ran and answers \p status.
std::function<BridgeStatus()> Recording(std::vector<std::string>& ran, std::string name,
                                        BridgeStatus status = BridgeStatus::Success) {
  return [&ran, name = std::move(name), status] {
    ran.push_back(name);
    return status;
  };
}

TEST(BrowserDeferredCalls, HeldCallsRunBeforeTheRoundTripThatCarriesThemInCallOrder) {
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  const auto origin = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;
  deferred.defer(origin, BrowserDeferredOperation::WriteBuffer, 1, Recording(ran, "write"));
  deferred.defer(origin, BrowserDeferredOperation::CreateBindGroup, 2, Recording(ran, "group"));
  deferred.defer(origin, BrowserDeferredOperation::DestroyObject, 3, Recording(ran, "destroy"));
  EXPECT_TRUE(ran.empty()) << "held calls wait for a round trip";
  EXPECT_EQ(channel.roundTrips(), 0u);

  deferred.run([&] { ran.emplace_back("submit"); }, 10);
  EXPECT_THAT(ran, ElementsAre("write", "group", "destroy", "submit"))
      << "a direct call runs after every call deferred before it";
  EXPECT_EQ(channel.roundTrips(), 1u) << "the held calls ride in the direct call's round trip";
  EXPECT_EQ(channel.lastKind(), 10u);
  EXPECT_TRUE(deferred.empty());

  deferred.run([&] { ran.emplace_back("create"); }, 4);
  EXPECT_THAT(ran, ElementsAre("write", "group", "destroy", "submit", "create"));
  EXPECT_EQ(channel.roundTrips(), 2u);
}

TEST(BrowserDeferredCalls, WhereCallsDoNotCrossThreadsTheyRunAtOnce) {
  FakeOwnerChannel channel;
  channel.crosses = false;
  BrowserDeferredCalls deferred(channel);
  const auto origin = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;
  EXPECT_EQ(deferred.defer(origin, BrowserDeferredOperation::WriteBuffer, 1,
                           Recording(ran, "write", BridgeStatus::UnknownObject)),
            BridgeStatus::UnknownObject)
      << "on the owner, or without one, the call answers for itself";
  EXPECT_THAT(ran, ElementsAre("write"));
  EXPECT_TRUE(deferred.empty());
  EXPECT_EQ(channel.roundTrips(), 0u);
}

TEST(BrowserDeferredCalls, AFlushOfAnEmptyQueueMakesNoRoundTrip) {
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  deferred.flush();
  EXPECT_EQ(channel.roundTrips(), 0u);
  std::vector<std::string> ran;
  deferred.defer(std::make_shared<BrowserDeferredOrigin>(), BrowserDeferredOperation::DestroyObject,
                 1, Recording(ran, "destroy"));
  deferred.flush();
  EXPECT_THAT(ran, ElementsAre("destroy"));
  EXPECT_EQ(channel.roundTrips(), 1u);
  EXPECT_EQ(channel.lastKind(), BrowserDeferredCalls::kCommandBatchKind);
}

TEST(BrowserDeferredCalls, WriteAndBindGroupRefusalsMarkOnlyTheirOwnDevice) {
  for (const BrowserDeferredOperation kind :
       {BrowserDeferredOperation::WriteBuffer, BrowserDeferredOperation::CreateBindGroup,
        BrowserDeferredOperation::CreateBindGroupLayout}) {
    SCOPED_TRACE(std::string(BrowserDeferredOperationName(kind)));
    FakeOwnerChannel channel;
    BrowserDeferredCalls deferred(channel);
    const auto refusing = std::make_shared<BrowserDeferredOrigin>();
    const auto other = std::make_shared<BrowserDeferredOrigin>();
    std::vector<std::string> ran;
    deferred.defer(other, BrowserDeferredOperation::WriteBuffer, 10, Recording(ran, "a"));
    deferred.defer(refusing, kind, 11, Recording(ran, "b", BridgeStatus::UnknownObject));
    deferred.defer(other, BrowserDeferredOperation::DestroyObject, 12, Recording(ran, "c"));

    const BrowserDeferredCalls::Report report = deferred.flush();
    EXPECT_THAT(ran, ElementsAre("a", "b", "c")) << "a refusal does not stop the calls after it";
    EXPECT_EQ(refusing->refusal.load(), static_cast<uint32_t>(BridgeStatus::UnknownObject));
    EXPECT_EQ(other->refusal.load(), 0u) << "no other device is charged with the refusal";
    EXPECT_EQ(report.operation, kind);
    EXPECT_EQ(report.objectId, 11u) << "the diagnostic names the refused call's own object";
    EXPECT_EQ(report.status, BridgeStatus::UnknownObject);
    EXPECT_EQ(report.refused, 1u);
  }
}

TEST(BrowserDeferredCalls, DestroyRefusalsAreCountedForDiagnosticsOnly) {
  for (const BridgeStatus status :
       {BridgeStatus::UnknownObject, BridgeStatus::WrongObjectKind, BridgeStatus::Failed}) {
    FakeOwnerChannel channel;
    BrowserDeferredCalls deferred(channel);
    const auto origin = std::make_shared<BrowserDeferredOrigin>();
    std::vector<std::string> ran;
    deferred.defer(origin, BrowserDeferredOperation::DestroyObject, 5,
                   Recording(ran, "destroy", status));
    bool submitted = false;
    const BrowserDeferredCalls::Report report =
        deferred.run([&] { submitted = true; }, 10, origin.get());
    EXPECT_EQ(origin->refusal.load(), 0u) << "a refused release does not make the device lost";
    EXPECT_EQ(origin->destroyRefusals.load(), 1u);
    EXPECT_EQ(report.refused, 1u);
    EXPECT_TRUE(submitted) << "a refused release does not stop the device's own call";
    EXPECT_FALSE(report.carriedCallSkipped);
  }
}

TEST(BrowserDeferredCalls, ADevicesFirstRefusalIsKept) {
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  const auto origin = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;
  deferred.defer(origin, BrowserDeferredOperation::CreateBindGroup, 1,
                 Recording(ran, "group", BridgeStatus::WrongObjectKind));
  deferred.defer(origin, BrowserDeferredOperation::WriteBuffer, 2,
                 Recording(ran, "write", BridgeStatus::DeviceLost));
  const BrowserDeferredCalls::Report report = deferred.flush();
  EXPECT_EQ(origin->refusal.load(), static_cast<uint32_t>(BridgeStatus::WrongObjectKind));
  EXPECT_EQ(report.refused, 2u);
}

TEST(BrowserDeferredCalls, ADevicesOwnCallDoesNotRunOverAWriteRefusedInTheSameTrip) {
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  const auto submitting = std::make_shared<BrowserDeferredOrigin>();
  const auto other = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;
  deferred.defer(submitting, BrowserDeferredOperation::WriteBuffer, 1,
                 Recording(ran, "write", BridgeStatus::Failed));
  BrowserDeferredCalls::Report report =
      deferred.run([&] { ran.emplace_back("submit"); }, 10, submitting.get());
  EXPECT_THAT(ran, ElementsAre("write")) << "the submission would run over stale contents";
  EXPECT_TRUE(report.carriedCallSkipped);

  // Another device's refusal does not stop this device's call.
  ran.clear();
  deferred.defer(other, BrowserDeferredOperation::WriteBuffer, 2,
                 Recording(ran, "other write", BridgeStatus::Failed));
  report = deferred.run([&] { ran.emplace_back("submit"); }, 10, submitting.get());
  EXPECT_THAT(ran, ElementsAre("other write", "submit"));
  EXPECT_FALSE(report.carriedCallSkipped);
}

TEST(BrowserDeferredCalls, MustBeSentOnceEitherBoundIsReached) {
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  const auto origin = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;
  for (std::size_t held = 1; held < BrowserDeferredCalls::kMaxCalls; ++held) {
    deferred.defer(origin, BrowserDeferredOperation::DestroyObject, held, Recording(ran, "d"));
  }
  EXPECT_EQ(channel.roundTrips(), 0u);
  deferred.defer(origin, BrowserDeferredOperation::DestroyObject, 0, Recording(ran, "d"));
  EXPECT_EQ(channel.roundTrips(), 1u) << "the call bound sends the held calls on their own";
  EXPECT_EQ(ran.size(), BrowserDeferredCalls::kMaxCalls);
  EXPECT_TRUE(deferred.empty());

  deferred.defer(origin, BrowserDeferredOperation::WriteBuffer, 1, Recording(ran, "w"),
                 BrowserDeferredCalls::kMaxBytes - 1);
  EXPECT_EQ(deferred.bytes(), BrowserDeferredCalls::kMaxBytes - 1);
  EXPECT_EQ(channel.roundTrips(), 1u);
  deferred.defer(origin, BrowserDeferredOperation::WriteBuffer, 2, Recording(ran, "w"), 1);
  EXPECT_EQ(channel.roundTrips(), 2u) << "the byte bound sends the held calls on their own";
  EXPECT_EQ(deferred.bytes(), 0u) << "sending releases the payload";
}

/// Defers a write whose held form counts how often it is built, as the browser bridge's write does.
BridgeStatus DeferCountedWrite(BrowserDeferredCalls& deferred,
                               const std::shared_ptr<BrowserDeferredOrigin>& origin,
                               std::span<const uint8_t> data, std::vector<std::string>& ran,
                               int& copies, BridgeStatus status = BridgeStatus::Success) {
  return deferred.defer(
      origin, BrowserDeferredOperation::WriteBuffer, 1,
      [&] {
        ran.emplace_back("write in place");
        return status;
      },
      [&] {
        ++copies;
        return BrowserDeferredCalls::OwningWrite(data, [&ran, status](std::span<const uint8_t>) {
          ran.emplace_back("held write");
          return status;
        });
      },
      data.size());
}

TEST(BrowserDeferredCalls, AWriteIsCopiedOnlyWhenItIsHeld) {
  const std::vector<uint8_t> small(16, 1);
  const auto origin = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;

  // Where calls do not cross threads, the write runs in place and nothing is copied.
  FakeOwnerChannel local;
  local.crosses = false;
  BrowserDeferredCalls direct(local);
  int copies = 0;
  EXPECT_EQ(DeferCountedWrite(direct, origin, small, ran, copies), BridgeStatus::Success);
  EXPECT_EQ(copies, 0);
  EXPECT_THAT(ran, ElementsAre("write in place"));

  // A held write is copied exactly once, and runs from the copy.
  ran.clear();
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  DeferCountedWrite(deferred, origin, small, ran, copies);
  EXPECT_EQ(copies, 1);
  deferred.flush();
  EXPECT_EQ(copies, 1);
  EXPECT_THAT(ran, ElementsAre("held write"));
}

TEST(BrowserDeferredCalls, AWriteAtTheByteBoundIsSentDirectlyWithoutACopy) {
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  const auto origin = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;
  deferred.defer(origin, BrowserDeferredOperation::DestroyObject, 1, Recording(ran, "destroy"));
  const std::vector<uint8_t> large(BrowserDeferredCalls::kMaxBytes, 2);
  int copies = 0;
  const BridgeStatus status =
      DeferCountedWrite(deferred, origin, large, ran, copies, BridgeStatus::Failed);
  EXPECT_EQ(status, BridgeStatus::Failed) << "a write sent directly answers for itself";
  EXPECT_EQ(copies, 0) << "the borrowed bytes are written, not a copy";
  EXPECT_THAT(ran, ElementsAre("destroy", "write in place"))
      << "it still runs after the calls held before it";
  EXPECT_EQ(channel.roundTrips(), 1u);
  EXPECT_TRUE(deferred.empty());
}

TEST(BrowserDeferredCalls, TheQueueKeepsItsCapacityAcrossRoundTrips) {
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  const auto origin = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;
  for (int round = 0; round < 3; ++round) {
    for (uint64_t id = 0; id < 64; ++id) {
      deferred.defer(origin, BrowserDeferredOperation::DestroyObject, id, Recording(ran, "d"));
    }
    deferred.flush();
  }
  const std::size_t settled = deferred.capacity();
  EXPECT_GE(settled, 64u) << "the held calls' storage is kept after sending them";
  for (uint64_t id = 0; id < 64; ++id) {
    deferred.defer(origin, BrowserDeferredOperation::DestroyObject, id, Recording(ran, "d"));
  }
  EXPECT_EQ(deferred.capacity(), settled) << "a frame of the same size reallocates nothing";
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

TEST(BrowserDeferredCalls, CallsOfAReleasedDeviceDoNotRunAndKeepTheirOriginAlive) {
  FakeOwnerChannel channel;
  BrowserDeferredCalls deferred(channel);
  auto origin = std::make_shared<BrowserDeferredOrigin>();
  const std::weak_ptr<BrowserDeferredOrigin> watched = origin;
  std::vector<std::string> ran;
  deferred.defer(origin, BrowserDeferredOperation::WriteBuffer, 1,
                 Recording(ran, "write", BridgeStatus::UnknownObject));
  deferred.defer(origin, BrowserDeferredOperation::DestroyObject, 2, Recording(ran, "destroy"));
  // The bridge releases its logical device and goes away while the calls are still held.
  origin->live.store(false);
  origin.reset();
  EXPECT_FALSE(watched.expired()) << "the held calls keep what they report to";

  const BrowserDeferredCalls::Report report = deferred.flush();
  EXPECT_TRUE(ran.empty()) << "nothing runs against a released logical device";
  EXPECT_EQ(report.skipped, 2u);
  EXPECT_EQ(report.refused, 0u);
  EXPECT_TRUE(watched.expired()) << "and nothing is kept once they are gone";
}

/// One thread's queue, as the browser bridge keeps one per thread.
BrowserDeferredCalls& ThreadQueue(FakeOwnerChannel& channel) {
  thread_local BrowserDeferredCalls queue(channel);
  return queue;
}

TEST(BrowserDeferredCalls, ADeviceTornDownOnAnotherThreadSkipsTheCallsItLeftHeld) {
  FakeOwnerChannel channel;
  auto origin = std::make_shared<BrowserDeferredOrigin>();
  std::atomic<int> ran{0};
  std::atomic<bool> held{false};
  std::atomic<bool> tornDown{false};
  std::thread owning([&] {
    BrowserDeferredCalls& queue = ThreadQueue(channel);
    queue.defer(origin, BrowserDeferredOperation::WriteBuffer, 1, [&] {
      ++ran;
      return BridgeStatus::Success;
    });
    held = true;
    while (!tornDown) {
      std::this_thread::yield();
    }
    queue.flush();
  });
  while (!held) {
    std::this_thread::yield();
  }
  // Teardown on this thread: release first, then let the bridge's reference go.
  origin->live.store(false);
  origin.reset();
  tornDown = true;
  owning.join();
  EXPECT_EQ(ran.load(), 0);
}

TEST(BrowserDeferredCalls, ThreadExitDropsWhatIsStillHeldWithoutARoundTrip) {
  FakeOwnerChannel channel;
  const auto live = std::make_shared<BrowserDeferredOrigin>();
  const auto released = std::make_shared<BrowserDeferredOrigin>();
  released->live.store(false);
  std::vector<std::string> ran;
  std::thread exiting([&] {
    BrowserDeferredCalls& queue = ThreadQueue(channel);
    // A thread flushes before it exits; what it holds after that is dropped, because a round trip
    // from thread exit could wait on an owner that is waiting to join this thread.
    queue.defer(live, BrowserDeferredOperation::DestroyObject, 1, Recording(ran, "flushed"));
    queue.flush();
    queue.defer(live, BrowserDeferredOperation::DestroyObject, 2, Recording(ran, "live destroy"));
    queue.defer(released, BrowserDeferredOperation::DestroyObject, 3,
                Recording(ran, "released destroy"));
  });
  exiting.join();
  EXPECT_THAT(ran, ElementsAre("flushed"));
  EXPECT_EQ(channel.roundTrips(), 1u) << "thread exit makes no round trip";
}

TEST(BrowserDeferredCalls, AWriteDroppedAtThreadExitMakesItsLiveDeviceFailClosed) {
  FakeOwnerChannel channel;
  const auto writing = std::make_shared<BrowserDeferredOrigin>();
  const auto building = std::make_shared<BrowserDeferredOrigin>();
  const auto releasing = std::make_shared<BrowserDeferredOrigin>();
  std::vector<std::string> ran;
  std::thread exiting([&] {
    BrowserDeferredCalls& queue = ThreadQueue(channel);
    queue.defer(writing, BrowserDeferredOperation::WriteBuffer, 1, Recording(ran, "write"));
    queue.defer(building, BrowserDeferredOperation::CreateBindGroup, 2, Recording(ran, "group"));
    queue.defer(releasing, BrowserDeferredOperation::DestroyObject, 3, Recording(ran, "destroy"));
  });
  exiting.join();
  EXPECT_TRUE(ran.empty());
  EXPECT_NE(writing->refusal.load(), 0u) << "a dropped write leaves its device out of step";
  EXPECT_NE(building->refusal.load(), 0u) << "a dropped bind group leaves no browser object";
  EXPECT_EQ(releasing->refusal.load(), 0u) << "a dropped release waits for device release";
  EXPECT_EQ(releasing->destroyRefusals.load(), 0u);
}

}  // namespace
}  // namespace donner::gpu::browser
